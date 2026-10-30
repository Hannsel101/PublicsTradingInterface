#include "telegraminspectorprocess.h"

#include <QFileInfo>
#include <QFile>
#include <QDir>
#include <QSaveFile>
#include <QLockFile>
#include <QJsonObject>
#include <QJsonArray>
#include <QJsonDocument>
#include <QCryptographicHash>
#include <QCoreApplication>
#include <QProcess>
#include <QStandardPaths>
#include <QVector>
#include <cstring>
#include <cerrno>
#ifdef Q_OS_UNIX
#include <sys/stat.h>
#endif

#ifdef Q_OS_WIN
#include <winsock2.h>
#include <qt_windows.h>
#include <iphlpapi.h>
#include <tlhelp32.h>
#include <winternl.h>
#include <shellapi.h>
#include <aclapi.h>
#endif

#ifdef Q_OS_MACOS
#include <libproc.h>
#include <sys/proc_info.h>
#include <sys/sysctl.h>
#include <arpa/inet.h>
#include <signal.h>
#include <unistd.h>
#endif

namespace {
QString executablePath(const QString &configured)
{
    const auto path = configured.contains('/') || configured.contains('\\')
        ? configured : QStandardPaths::findExecutable(configured);
    const QFileInfo info(path);
    return info.isFile() && info.isExecutable() ? info.canonicalFilePath() : QString{};
}
#ifdef Q_OS_MACOS
bool processInfo(int pid, proc_bsdinfo &info)
{
    return proc_pidinfo(pid, PROC_PIDTBSDINFO, 0, &info, sizeof(info)) == sizeof(info);
}
QString imagePath(int pid)
{
    char path[PROC_PIDPATHINFO_MAXSIZE] = {};
    if (proc_pidpath(pid, path, sizeof(path)) <= 0) return {};
    return QFileInfo(QString::fromLocal8Bit(path)).canonicalFilePath();
}
bool argumentsMatch(int pid, const QStringList &expected)
{
    // KERN_PROCARGS2 includes environment after argv. Parse only argc strings;
    // never retain, log or inspect the environment values.
    int mib[] = { CTL_KERN, KERN_PROCARGS2, pid };
    QByteArray data(256 * 1024, Qt::Uninitialized);
    size_t size = size_t(data.size());
    if (sysctl(mib, 3, data.data(), &size, nullptr, 0) != 0 || size < sizeof(int)) return false;
    int count = 0;
    std::memcpy(&count, data.constData(), sizeof(count));
    if (count != expected.size() + 1) return false;
    const char *cursor = data.constData() + sizeof(int);
    const char *end = data.constData() + size;
    auto skipString = [&]() {
        const auto *zero = static_cast<const char *>(std::memchr(cursor, 0, size_t(end - cursor)));
        if (!zero) return false;
        cursor = zero + 1; return true;
    };
    if (!skipString()) return false; // executable path preceding argv
    while (cursor < end && !*cursor) ++cursor;
    if (!skipString()) return false; // argv[0]
    for (const auto &arg : expected) {
        const char *begin = cursor;
        if (!skipString() || QString::fromLocal8Bit(begin, cursor - begin - 1) != arg) return false;
    }
    return true;
}
bool ownsListener(int pid, quint16 port)
{
    int bytes = proc_pidinfo(pid, PROC_PIDLISTFDS, 0, nullptr, 0);
    if (bytes <= 0 || bytes > 1024 * 1024) return false;
    QVector<proc_fdinfo> fds(bytes / int(sizeof(proc_fdinfo)) + 16);
    bytes = proc_pidinfo(pid, PROC_PIDLISTFDS, 0, fds.data(), int(fds.size() * sizeof(proc_fdinfo)));
    if (bytes <= 0 || bytes >= fds.size() * qsizetype(sizeof(proc_fdinfo))) return false;
    int matches = 0;
    for (int i = 0; i < bytes / int(sizeof(proc_fdinfo)); ++i) {
        if (fds[i].proc_fdtype != PROX_FDTYPE_SOCKET) continue;
        socket_fdinfo socket{};
        if (proc_pidfdinfo(pid, fds[i].proc_fd, PROC_PIDFDSOCKETINFO, &socket, sizeof(socket)) != sizeof(socket)) continue;
        const auto &tcp = socket.psi.soi_proto.pri_tcp;
        if (socket.psi.soi_family != AF_INET || socket.psi.soi_kind != SOCKINFO_TCP
            || tcp.tcpsi_state != TSI_S_LISTEN || ntohs(quint16(tcp.tcpsi_ini.insi_lport)) != port) continue;
        // Wildcard/external bind is never accepted as the private inspector.
        if (tcp.tcpsi_ini.insi_laddr.ina_46.i46a_addr4.s_addr != htonl(INADDR_LOOPBACK)) return false;
        ++matches;
    }
    return matches == 1;
}
#endif
#ifdef Q_OS_WIN
struct HandleCloser {
    void operator()(void *handle) const { if (handle && handle != INVALID_HANDLE_VALUE) CloseHandle(HANDLE(handle)); }
};
using NativeHandle = std::unique_ptr<void, HandleCloser>;
quint64 fileTime(const FILETIME &time)
{
    return (quint64(time.dwHighDateTime) << 32) | time.dwLowDateTime;
}
quint64 creationTime(HANDLE process)
{
    FILETIME created{}, exited{}, kernel{}, user{};
    return GetProcessTimes(process, &created, &exited, &kernel, &user) ? fileTime(created) : 0;
}
bool sameUser(HANDLE process)
{
    HANDLE ownRaw = nullptr, otherRaw = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &ownRaw)) return false;
    NativeHandle own(ownRaw);
    if (!OpenProcessToken(process, TOKEN_QUERY, &otherRaw)) return false;
    NativeHandle other(otherRaw);
    auto tokenUser = [](HANDLE token) {
        DWORD size = 0;
        GetTokenInformation(token, TokenUser, nullptr, 0, &size);
        if (!size || size > 65536) return QByteArray{};
        QByteArray buffer(size, Qt::Uninitialized);
        if (!GetTokenInformation(token, TokenUser, buffer.data(), size, &size)) return QByteArray{};
        return buffer;
    };
    const auto a = tokenUser(HANDLE(own.get())), b = tokenUser(HANDLE(other.get()));
    return !a.isEmpty() && !b.isEmpty()
        && EqualSid(reinterpret_cast<const TOKEN_USER *>(a.constData())->User.Sid,
                    reinterpret_cast<const TOKEN_USER *>(b.constData())->User.Sid);
}
QString imagePath(HANDLE process)
{
    wchar_t image[32768] = {};
    DWORD size = 32768;
    if (!QueryFullProcessImageNameW(process, 0, image, &size)) return {};
    return QFileInfo(QString::fromWCharArray(image, int(size))).canonicalFilePath();
}
bool argumentsMatch(HANDLE process, const QStringList &expected)
{
    // Read the documented same-architecture PEB/RTL_USER_PROCESS_PARAMETERS
    // layout, not a shell command, WMI provider or unbounded command output.
    using MachineInfo = BOOL (WINAPI *)(HANDLE, USHORT *, USHORT *);
    const auto machineInfo = reinterpret_cast<MachineInfo>(
        GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "IsWow64Process2"));
    USHORT ownMachine = 0, ownNative = 0, otherMachine = 0, otherNative = 0;
    if (!machineInfo || !machineInfo(GetCurrentProcess(), &ownMachine, &ownNative)
        || !machineInfo(process, &otherMachine, &otherNative)
        || ownMachine != otherMachine || ownNative != otherNative) return false;
    using QueryInfo = NTSTATUS (NTAPI *)(HANDLE, PROCESSINFOCLASS, PVOID, ULONG, PULONG);
    const auto query = reinterpret_cast<QueryInfo>(
        GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtQueryInformationProcess"));
    PROCESS_BASIC_INFORMATION basic{};
    if (!query || query(process, ProcessBasicInformation, &basic, sizeof(basic), nullptr) < 0) return false;
    const auto read = [process](const void *address, void *buffer, SIZE_T size) {
        SIZE_T received = 0;
        return address && ReadProcessMemory(process, address, buffer, size, &received) && received == size;
    };
    PEB peb{};
    RTL_USER_PROCESS_PARAMETERS params{};
    if (!read(basic.PebBaseAddress, &peb, sizeof(peb))
        || !read(peb.ProcessParameters, &params, sizeof(params))) return false;
    const auto &line = params.CommandLine;
    if (!line.Buffer || !line.Length || line.Length % sizeof(wchar_t)
        || line.Length > line.MaximumLength || line.Length > 65534) return false;
    QVector<wchar_t> text(line.Length / sizeof(wchar_t) + 1, L'\0');
    if (!read(line.Buffer, text.data(), line.Length)) return false;
    int count = 0;
    auto **argv = CommandLineToArgvW(text.constData(), &count);
    if (!argv) return false;
    bool match = count == expected.size() + 1;
    for (int i = 1; match && i < count; ++i)
        match = QString::fromWCharArray(argv[i]) == expected[i - 1];
    LocalFree(argv);
    return match;
}
DWORD listenerPid(quint16 port)
{
    DWORD size = 0;
    if (GetExtendedTcpTable(nullptr, &size, FALSE, AF_INET, TCP_TABLE_OWNER_PID_LISTENER, 0)
            != ERROR_INSUFFICIENT_BUFFER || size > 4 * 1024 * 1024) return 0;
    QByteArray bytes(size, Qt::Uninitialized);
    if (GetExtendedTcpTable(bytes.data(), &size, FALSE, AF_INET, TCP_TABLE_OWNER_PID_LISTENER, 0) != NO_ERROR) return 0;
    const auto *table = reinterpret_cast<const MIB_TCPTABLE_OWNER_PID *>(bytes.constData());
    if (size < sizeof(DWORD) || table->dwNumEntries > (size - sizeof(DWORD)) / sizeof(MIB_TCPROW_OWNER_PID)) return 0;
    DWORD pid = 0;
    for (DWORD i = 0; i < table->dwNumEntries; ++i) {
        const auto &row = table->table[i];
        if (ntohs(u_short(row.dwLocalPort)) != port) continue;
        if (row.dwLocalAddr != htonl(INADDR_LOOPBACK) || pid) return 0;
        pid = row.dwOwningPid;
    }
    return pid;
}
#endif

constexpr qsizetype MaxReceiptBytes = 8192;
constexpr int MaxReceipts = 64;
QString receiptChecksum(QJsonObject object)
{
    object.remove("receiptSha256");
    return QString::fromLatin1(QCryptographicHash::hash(
        QJsonDocument(object).toJson(QJsonDocument::Compact), QCryptographicHash::Sha256).toHex());
}
QString argvDigest(const QStringList &args)
{
    return QString::fromLatin1(QCryptographicHash::hash(
        QJsonDocument(QJsonArray::fromStringList(args)).toJson(QJsonDocument::Compact),
        QCryptographicHash::Sha256).toHex());
}
#ifdef Q_OS_WIN
QByteArray userSid()
{
    HANDLE raw = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &raw)) return {};
    NativeHandle token(raw);
    DWORD size = 0;
    GetTokenInformation(raw, TokenUser, nullptr, 0, &size);
    if (!size || size > 65536) return {};
    QByteArray data(size, Qt::Uninitialized);
    if (!GetTokenInformation(raw, TokenUser, data.data(), size, &size)) return {};
    auto *sid = reinterpret_cast<TOKEN_USER *>(data.data())->User.Sid;
    return QByteArray(static_cast<const char *>(sid), GetLengthSid(sid));
}
#endif
QString userIdentity()
{
#ifdef Q_OS_MACOS
    return QString::number(getuid());
#elif defined(Q_OS_WIN)
    return QString::fromLatin1(userSid().toHex());
#else
    return {};
#endif
}
// Refuse foreign-owned, symlinked, public or oversized storage. This directory
// is an ownership boundary, not a cache that can be repaired from process metadata.
bool privatePath(const QString &path, bool directory, bool initialize = false)
{
    const QFileInfo info(path);
    if (info.isSymLink() || (directory ? !info.isDir() : !info.isFile())) return false;
#ifdef Q_OS_MACOS
    struct stat st{};
    if (::lstat(QFile::encodeName(path).constData(), &st) != 0 || st.st_uid != getuid()) return false;
    if (initialize && ::chmod(QFile::encodeName(path).constData(), directory ? 0700 : 0600) != 0) return false;
    if (::lstat(QFile::encodeName(path).constData(), &st) != 0) return false;
    return (st.st_mode & 0777) == (directory ? 0700 : 0600) && (directory || st.st_nlink == 1);
#elif defined(Q_OS_WIN)
    const auto sid = userSid();
    if (sid.isEmpty() || (GetFileAttributesW(reinterpret_cast<LPCWSTR>(path.utf16())) & FILE_ATTRIBUTE_REPARSE_POINT)) return false;
    if (initialize) {
        EXPLICIT_ACCESSW access{};
        access.grfAccessPermissions = GENERIC_ALL;
        access.grfAccessMode = SET_ACCESS;
        access.grfInheritance = directory ? SUB_CONTAINERS_AND_OBJECTS_INHERIT : NO_INHERITANCE;
        access.Trustee.TrusteeForm = TRUSTEE_IS_SID;
        access.Trustee.TrusteeType = TRUSTEE_IS_USER;
        access.Trustee.ptstrName = reinterpret_cast<LPWSTR>(const_cast<char *>(sid.constData()));
        PACL acl = nullptr;
        if (SetEntriesInAclW(1, &access, nullptr, &acl) != ERROR_SUCCESS) return false;
        const auto result = SetNamedSecurityInfoW(const_cast<LPWSTR>(reinterpret_cast<LPCWSTR>(path.utf16())),
            SE_FILE_OBJECT, DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION,
            nullptr, nullptr, acl, nullptr);
        LocalFree(acl);
        if (result != ERROR_SUCCESS) return false;
    }
    PSID owner = nullptr; PACL acl = nullptr; PSECURITY_DESCRIPTOR descriptor = nullptr;
    if (GetNamedSecurityInfoW(reinterpret_cast<LPCWSTR>(path.utf16()), SE_FILE_OBJECT,
            OWNER_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION, &owner, nullptr, &acl, nullptr,
            &descriptor) != ERROR_SUCCESS) return false;
    bool valid = owner && EqualSid(owner, const_cast<char *>(sid.constData())) && acl && acl->AceCount == 1;
    void *raw = nullptr;
    if (valid) valid = GetAce(acl, 0, &raw);
    if (valid) {
        const auto *ace = static_cast<const ACCESS_ALLOWED_ACE *>(raw);
        valid = ace->Header.AceType == ACCESS_ALLOWED_ACE_TYPE
            && EqualSid(const_cast<DWORD *>(&ace->SidStart), const_cast<char *>(sid.constData()));
    }
    LocalFree(descriptor);
    return valid;
#else
    Q_UNUSED(initialize);
    return false;
#endif
}
QString receiptRoot(const QString &configured, bool create)
{
    const auto root = configured.isEmpty()
        ? QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation)
            + QStringLiteral("/telegram-inspector-ownership-v1") : configured;
    if (!QDir::isAbsolutePath(root)) return {};
    const bool exists = QFileInfo::exists(root);
    if (!exists && (!create || !QDir().mkpath(root))) return {};
    return privatePath(root, true, !exists) ? root : QString{};
}
QString receiptPath(const QString &root, quint16 port)
{
    return QDir(root).filePath(QStringLiteral("inspector-%1.json").arg(port));
}
QByteArray readReceipt(const QString &root, quint16 port)
{
    const auto path = receiptPath(root, port);
    if (!privatePath(path, false) || QFileInfo(path).size() > MaxReceiptBytes) return {};
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) return {};
    const auto bytes = file.read(MaxReceiptBytes + 1);
    return bytes.size() <= MaxReceiptBytes ? bytes : QByteArray{};
}
struct Birth {
    quint64 a = 0, b = 0;
    bool operator==(const Birth &) const = default;
};
bool nativeBirth(qint64 pid, Birth &birth, qint64 *parent = nullptr)
{
    if (pid <= 1 || pid > 0x7fffffff) return false;
#ifdef Q_OS_MACOS
    proc_bsdinfo info{};
    if (!processInfo(int(pid), info) || info.pbi_uid != getuid() || info.pbi_ruid != getuid()
        || info.pbi_svuid != getuid()) return false;
    birth = {info.pbi_start_tvsec, info.pbi_start_tvusec};
    if (parent) *parent = info.pbi_ppid;
    return birth.a != 0;
#elif defined(Q_OS_WIN)
    NativeHandle handle(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE, FALSE, DWORD(pid)));
    if (!handle || !sameUser(HANDLE(handle.get()))) return false;
    birth = {creationTime(HANDLE(handle.get())), 0};
    if (parent) {
        NativeHandle snapshot(CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0));
        if (snapshot.get() == INVALID_HANDLE_VALUE) return false;
        PROCESSENTRY32W entry{}; entry.dwSize = sizeof(entry);
        bool found = false;
        if (Process32FirstW(HANDLE(snapshot.get()), &entry)) do {
            if (entry.th32ProcessID == DWORD(pid)) { *parent = entry.th32ParentProcessID; found = true; break; }
        } while (Process32NextW(HANDLE(snapshot.get()), &entry));
        if (!found) return false;
    }
    return birth.a != 0;
#else
    Q_UNUSED(birth); Q_UNUSED(parent);
    return false;
#endif
}
bool creatorGone(qint64 pid, Birth original, Birth child)
{
    if (pid <= 1 || !original.a || original.a > child.a
        || (original.a == child.a && original.b > child.b)) return false;
#ifdef Q_OS_MACOS
    proc_bsdinfo info{};
    errno = 0;
    if (!processInfo(int(pid), info)) return errno == ESRCH;
    const Birth current{info.pbi_start_tvsec, info.pbi_start_tvusec};
    return current != original && (current.a > child.a || (current.a == child.a && current.b > child.b));
#elif defined(Q_OS_WIN)
    NativeHandle handle(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE, FALSE, DWORD(pid)));
    if (!handle) return GetLastError() == ERROR_INVALID_PARAMETER;
    const auto current = creationTime(HANDLE(handle.get()));
    return current && current != original.a && current > child.a;
#else
    return false;
#endif
}
bool parseUnsigned(const QJsonObject &object, const char *key, quint64 &result)
{
    const auto value = object.value(QLatin1String(key));
    if (!value.isString()) return false;
    const auto text = value.toString();
    bool ok = false;
    result = text.toULongLong(&ok);
    return ok && QString::number(result) == text;
}
}

struct TelegramInspectorProcess::State
{
    QString executable;
    QStringList arguments;
    quint16 port = 0;
    QString root;
    QByteArray receipt;
    qint64 creator = 0;
    Birth creatorBirth;
    bool ownership() const
    {
        return !root.isEmpty() && privatePath(root, true)
            && readReceipt(root, port) == receipt
#ifdef Q_OS_MACOS
            && creatorGone(creator, creatorBirth, {seconds, micros});
#elif defined(Q_OS_WIN)
            && creatorGone(creator, creatorBirth, {born, 0});
#else
            && false;
#endif
    }
#ifdef Q_OS_MACOS
    int pid = 0;
    quint64 seconds = 0;
    quint64 micros = 0;
    bool identity() const
    {
        Birth current;
        return nativeBirth(pid, current) && current == Birth{seconds, micros};
    }
    bool verified() const
    {
        return ownership() && identity() && imagePath(pid) == executable && argumentsMatch(pid, arguments)
            && ownsListener(pid, port);
    }
#elif defined(Q_OS_WIN)
    NativeHandle process;
    DWORD pid = 0;
    quint64 born = 0;
    bool identity() const
    {
        return process && WaitForSingleObject(HANDLE(process.get()), 0) == WAIT_TIMEOUT
            && creationTime(HANDLE(process.get())) == born;
    }
    bool verified() const
    {
        const auto handle = HANDLE(process.get());
        return ownership() && identity() && sameUser(handle)
            && imagePath(handle).compare(executable, Qt::CaseInsensitive) == 0
            && argumentsMatch(handle, arguments) && listenerPid(port) == pid;
    }
#endif
};
TelegramInspectorProcess::TelegramInspectorProcess(std::unique_ptr<State> state) : d(std::move(state)) {}
TelegramInspectorProcess::~TelegramInspectorProcess() = default;
QByteArray TelegramInspectorProcess::recordStarted(const QProcess &child, quint16 inspectorPort,
    const QString &ownershipDirectory)
{
    const auto pid = child.processId(), creator = QCoreApplication::applicationPid();
    Birth born, ownerBorn;
    qint64 parent = 0;
    const auto expected = executablePath(child.program());
    if (!inspectorPort || child.state() != QProcess::Running || expected.isEmpty()
        || !nativeBirth(pid, born, &parent) || parent != creator || !nativeBirth(creator, ownerBorn)) return {};
#ifdef Q_OS_MACOS
    if (imagePath(int(pid)) != expected || !argumentsMatch(int(pid), child.arguments())) return {};
#elif defined(Q_OS_WIN)
    NativeHandle handle(OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ | SYNCHRONIZE, FALSE, DWORD(pid)));
    if (!handle || imagePath(HANDLE(handle.get())).compare(expected, Qt::CaseInsensitive)
        || !argumentsMatch(HANDLE(handle.get()), child.arguments())) return {};
#else
    return {};
#endif
    const auto root = receiptRoot(ownershipDirectory, true);
    if (root.isEmpty()) return {};
    QLockFile lock(QDir(root).filePath("receipts.lock"));
    if (!lock.tryLock(0)) return {};
    const auto path = receiptPath(root, inspectorPort);
    // A private, valid receipt for a definitely exited instance must not block a
    // fresh app-owned child. Never repair corrupt records or adopt a live PID.
    if (QFileInfo::exists(path)) {
        const auto previous = readReceipt(root, inspectorPort);
        const auto document = QJsonDocument::fromJson(previous);
        const auto object = document.object();
        quint64 previousPid = 0;
        Birth previousBirth;
        if (!document.isObject() || document.toJson(QJsonDocument::Compact) != previous
            || object.size() != 12 || object.value("schema") != QJsonValue(1)
            || object.value("receiptSha256").toString() != receiptChecksum(object)
            || object.value("user").toString() != userIdentity()
            || object.value("port") != QJsonValue(int(inspectorPort))
            || !parseUnsigned(object, "pid", previousPid) || previousPid <= 1 || previousPid > 0x7fffffff
            || !parseUnsigned(object, "birth", previousBirth.a)
            || !parseUnsigned(object, "birthFraction", previousBirth.b)
            || !creatorGone(qint64(previousPid), previousBirth, born)
            || !QFile::remove(path)) return {};
    }
    if (QDir(root).entryList({"inspector-*.json"}, QDir::Files).size() >= MaxReceipts) return {};
    QJsonObject object{{"schema", 1}, {"pid", QString::number(pid)},
        {"birth", QString::number(born.a)}, {"birthFraction", QString::number(born.b)},
        {"creatorPid", QString::number(creator)}, {"creatorBirth", QString::number(ownerBorn.a)},
        {"creatorBirthFraction", QString::number(ownerBorn.b)}, {"user", userIdentity()},
        {"executable", expected}, {"argvSha256", argvDigest(child.arguments())}, {"port", int(inspectorPort)}};
    // Corruption detection only: the private directory/owner ACL is the trust boundary.
    object.insert("receiptSha256", receiptChecksum(object));
    const auto receipt = QJsonDocument(object).toJson(QJsonDocument::Compact);
    if (receipt.size() > MaxReceiptBytes || userIdentity().isEmpty()) return {};
    QSaveFile file(path);
    file.setDirectWriteFallback(false);
    if (!file.open(QIODevice::WriteOnly)
        || !file.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner)
        || file.write(receipt) != receipt.size()) return {};
    // Recheck the native child after preparing the atomic write.
    Birth current;
    if (child.state() != QProcess::Running || !nativeBirth(pid, current, &parent)
        || current != born || parent != creator || !file.commit()) return {};
    if (!privatePath(path, false) || readReceipt(root, inspectorPort) != receipt) {
        QFile::remove(path);
        return {};
    }
    return receipt;
}
void TelegramInspectorProcess::forgetReceipt(quint16 port, const QString &ownershipDirectory,
    const QByteArray &receipt)
{
    if (receipt.isEmpty()) return;
    const auto root = receiptRoot(ownershipDirectory, false);
    if (root.isEmpty()) return;
    QLockFile lock(QDir(root).filePath("receipts.lock"));
    if (lock.tryLock(0) && readReceipt(root, port) == receipt) QFile::remove(receiptPath(root, port));
}
std::unique_ptr<TelegramInspectorProcess> TelegramInspectorProcess::findOrphan(
    const QString &executable, const QStringList &arguments, quint16 inspectorPort,
    const QString &ownershipDirectory)
{
    // Read only this app's positive receipt. Never scan/claim a legacy process,
    // even when its UID, argv, executable, listener and PPID all match.
    const auto root = receiptRoot(ownershipDirectory, false);
    if (root.isEmpty() || !inspectorPort) return {};
    const auto receipt = readReceipt(root, inspectorPort);
    const auto document = QJsonDocument::fromJson(receipt);
    const auto object = document.object();
    const auto expected = executablePath(executable);
    quint64 pid = 0, creator = 0;
    Birth born, creatorBirth;
    if (!document.isObject() || document.toJson(QJsonDocument::Compact) != receipt
        || object.size() != 12 || object.value("schema") != QJsonValue(1)
        || object.value("receiptSha256").toString() != receiptChecksum(object)
        || expected.isEmpty() || object.value("executable").toString() != expected
        || object.value("user").toString() != userIdentity() || userIdentity().isEmpty()
        || object.value("argvSha256").toString() != argvDigest(arguments)
        || object.value("port") != QJsonValue(int(inspectorPort))
        || !parseUnsigned(object, "pid", pid) || pid <= 1 || pid > 0x7fffffff
        || !parseUnsigned(object, "creatorPid", creator) || creator <= 1 || creator > 0x7fffffff
        || pid == creator || !parseUnsigned(object, "birth", born.a) || !born.a
        || !parseUnsigned(object, "birthFraction", born.b)
        || !parseUnsigned(object, "creatorBirth", creatorBirth.a)
        || !parseUnsigned(object, "creatorBirthFraction", creatorBirth.b)
        || !creatorGone(qint64(creator), creatorBirth, born)) return {};
#if defined(Q_OS_MACOS) || defined(Q_OS_WIN)
    auto state = std::make_unique<State>();
    state->executable = expected; state->arguments = arguments; state->port = inspectorPort;
    state->root = root; state->receipt = receipt;
    state->creator = qint64(creator); state->creatorBirth = creatorBirth;
#ifdef Q_OS_MACOS
    state->pid = int(pid); state->seconds = born.a; state->micros = born.b;
#else
    state->pid = DWORD(pid); state->born = born.a;
    if (born.b) return {};
    state->process.reset(OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ | PROCESS_TERMINATE | SYNCHRONIZE, FALSE, DWORD(pid)));
    if (!state->process) return {};
#endif
    if (!state->verified()) return {};
    return std::unique_ptr<TelegramInspectorProcess>(new TelegramInspectorProcess(std::move(state)));
#else
    return {}; // Unsupported platforms fail closed.
#endif
}
bool TelegramInspectorProcess::terminate(bool force) const
{
#ifdef Q_OS_MACOS
    // Darwin has no pidfd signal API: check the start timestamp again as the
    // final operation before kill(), and never signal a PID on failed proof.
    if (!d->verified() || !d->identity()) return false;
    return ::kill(d->pid, force ? SIGKILL : SIGTERM) == 0;
#elif defined(Q_OS_WIN)
    if (!d->verified() || !d->identity()) return false;
    if (force) return TerminateProcess(HANDLE(d->process.get()), 1);
    // QProcess's graceful Windows stop is WM_CLOSE. Post rather than Send so
    // an unresponsive window cannot block the application; console-only agents
    // get the same bounded grace period before verified handle-based escalation.
    struct CloseContext { DWORD pid; HANDLE process; } context{d->pid, HANDLE(d->process.get())};
    EnumWindows([](HWND window, LPARAM value) -> BOOL {
        const auto *context = reinterpret_cast<const CloseContext *>(value);
        DWORD pid = 0;
        GetWindowThreadProcessId(window, &pid);
        if (pid == context->pid && WaitForSingleObject(context->process, 0) == WAIT_TIMEOUT)
            PostMessageW(window, WM_CLOSE, 0, 0);
        return TRUE;
    }, reinterpret_cast<LPARAM>(&context));
    return true;
#else
    Q_UNUSED(force);
    return false;
#endif
}
void TelegramInspectorProcess::forgetExitedReceipt() const
{
    if (!alive()) forgetReceipt(d->port, d->root, d->receipt);
}
bool TelegramInspectorProcess::alive() const
{
#if defined(Q_OS_MACOS) || defined(Q_OS_WIN)
    return d->identity();
#else
    return false;
#endif
}
