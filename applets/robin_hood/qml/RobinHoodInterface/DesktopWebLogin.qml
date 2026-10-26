import QtQuick
import QtWebEngine

WebEngineView {
    id: root
    objectName: "robinDesktopWebLogin"
    profile: WebEngineProfile {
        id: loginProfile
        offTheRecord: true
        persistentCookiesPolicy: WebEngineProfile.NoPersistentCookies
        httpCacheType: WebEngineProfile.MemoryHttpCache
    }
    url: RobinBrowser.startUrl

    Component.onCompleted: RobinBrowser.attachProfile(loginProfile)
    Component.onDestruction: RobinBrowser.detachProfile(loginProfile)
    onUrlChanged: RobinBrowser.navigationChanged(root.url)
    onNavigationRequested: request => {
        if (!request.isMainFrame || RobinBrowser.isAllowedNavigation(request.url))
            request.accept()
        else
            request.reject()
    }
    onCertificateError: error => {
        error.rejectCertificate()
        RobinBrowser.pageFailed()
    }
    onLoadingChanged: loadingInfo => {
        if (loadingInfo.status === WebEngineView.LoadFailedStatus)
            RobinBrowser.pageFailed()
    }
    // Not accepting a new-window request leaves it rejected.
    onNewWindowRequested: request => {}
}
