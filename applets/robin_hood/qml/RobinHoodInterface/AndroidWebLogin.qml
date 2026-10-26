import QtQuick
import QtWebView as NativeWebView

NativeWebView.WebView {
    id: root
    objectName: "robinAndroidWebLogin"
    url: RobinBrowser.startUrl
    onUrlChanged: RobinBrowser.navigationChanged(root.url)
    onLoadingChanged: loadRequest => {
        if (loadRequest.status === NativeWebView.WebView.LoadFailedStatus)
            RobinBrowser.pageFailed()
    }
}
