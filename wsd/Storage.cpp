/* -*- Mode: C++; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 4; fill-column: 100 -*- */
/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#include <chrono>
#include <config.h>

#include "HttpRequest.hpp"
#include "Storage.hpp"

#include <algorithm>
#include <memory>
#include <cassert>
#include <errno.h>
#include <fstream>
#include <iconv.h>
#include <string>

#include <Poco/Exception.h>
#include <Poco/JSON/Object.h>
#include <Poco/JSON/Parser.h>

#if !MOBILEAPP

#include <Poco/Net/AcceptCertificateHandler.h>
#include <Poco/Net/Context.h>
#include <Poco/Net/DNS.h>
#include <Poco/Net/HTTPClientSession.h>
#include <Poco/Net/HTTPRequest.h>
#include <Poco/Net/HTTPResponse.h>
#include <Poco/Net/HTTPSClientSession.h>
#include <Poco/Net/KeyConsoleHandler.h>
#include <Poco/Net/NameValueCollection.h>
#include <Poco/Net/SSLManager.h>

#endif

#include <Poco/StreamCopier.h>
#include <Poco/URI.h>

#include "Auth.hpp"
#include <Common.hpp>
#include "Exceptions.hpp"
#include <Log.hpp>
#include <Unit.hpp>
#include <Util.hpp>
#include "ProofKey.hpp"
#include <common/FileUtil.hpp>
#include <common/JsonUtil.hpp>
#include <common/TraceEvent.hpp>
#include <NetUtil.hpp>
#include <CommandControl.hpp>

#ifdef IOS
#include <ios.h>
#elif defined(__ANDROID__)
#include "androidapp.hpp"
#endif

#if !defined(BUILDING_TESTS)
bool StorageBase::FilesystemEnabled;
bool StorageBase::WopiEnabled;
bool StorageBase::SSLAsScheme = true;
bool StorageBase::SSLEnabled = false;
Util::RegexListMatcher StorageBase::WopiHosts;
std::map<std::string, std::string> StorageBase::AliasHosts;
std::set<std::string> StorageBase::AllHosts;
std::string StorageBase::FirstHost;
#endif // !defined(BUILDING_TESTS)

#if !MOBILEAPP

std::string StorageBase::getLocalRootPath() const
{
    std::string localPath = _jailPath;
    if (localPath[0] == '/')
    {
        // Remove the leading /
        localPath.erase(0, 1);
    }

    // /chroot/jailId/user/doc/childId
    const Poco::Path rootPath = Poco::Path(_localStorePath, localPath);
    Poco::File(rootPath).createDirectories();

    return rootPath.toString();
}

#if !defined(BUILDING_TESTS)
void StorageBase::parseWopiHost(Poco::Util::LayeredConfiguration& conf)
{
    // Parse the WOPI settings.
    WopiHosts.clear();
    WopiEnabled = conf.getBool("storage.wopi[@allow]", false);
    if (WopiEnabled)
    {
        for (size_t i = 0;; ++i)
        {
            const std::string path = "storage.wopi.host[" + std::to_string(i) + ']';
            if (!conf.has(path))
            {
                break;
            }
            StorageBase::addWopiHost(conf.getString(path, ""), conf.getBool(path + "[@allow]", false));
        }
    }
}

void StorageBase::addWopiHost(std::string host, bool allow)
{
    if (!host.empty())
    {
        if (allow)
        {
            LOG_INF("Adding trusted WOPI host: [" << host << "].");
            WopiHosts.allow(host);
        }
        else
        {
            LOG_INF("Adding blocked WOPI host: [" << host << "].");
            WopiHosts.deny(host);
        }
    }
}

void StorageBase::parseAliases(Poco::Util::LayeredConfiguration& conf)
{
    //set alias_groups mode to compat
    if (!conf.has("storage.wopi.alias_groups"))
    {
        conf.setString("storage.wopi.alias_groups[@mode]", "compat");
    }
    else if (conf.has("storage.wopi.alias_groups.group[0]"))
    {
        // group defined in alias_groups
        if (Util::iequal(config::getString("storage.wopi.alias_groups[@mode]", "first"), "first"))
        {
            LOG_ERR("Admins didnot set the alias_groups mode to 'groups'");
            AliasHosts.clear();
            AllHosts.clear();
            return;
        }
    }

    AliasHosts.clear();
    AllHosts.clear();

    for (size_t i = 0;; i++)
    {
        const std::string path = "storage.wopi.alias_groups.group[" + std::to_string(i) + ']';
        if (!conf.has(path + ".host"))
        {
            break;
        }

        const std::string uri = conf.getString(path + ".host", "");
        if (uri.empty())
        {
            continue;
        }
        bool allow = conf.getBool(path + ".host[@allow]", false);

        try
        {
            const Poco::URI realUri(uri);
            StorageBase::addWopiHost(realUri.getHost(), allow);
            AllHosts.insert(realUri.getAuthority());
        }
        catch (const Poco::Exception& exc)
        {
            LOG_WRN("parseAliases: " << exc.displayText());
        }

        for (size_t j = 0;; j++)
        {
            const std::string aliasPath = path + ".alias[" + std::to_string(j) + ']';
            if (!conf.has(aliasPath))
            {
                break;
            }

            try
            {
                const Poco::URI aliasUri(conf.getString(aliasPath, ""));
                if (aliasUri.empty())
                {
                    continue;
                }
                const Poco::URI realUri(uri);
                AliasHosts.insert({ aliasUri.getAuthority(), realUri.getAuthority() });
                AllHosts.insert(aliasUri.getAuthority());
                StorageBase::addWopiHost(aliasUri.getHost(), allow);
            }
            catch (const Poco::Exception& exc)
            {
                LOG_WRN("parseAliases: " << exc.displayText());
            }
        }
    }
}

std::string StorageBase::getNewUri(const Poco::URI& uri)
{
    if (Util::iequal(config::getString("storage.wopi.alias_groups[@mode]", "first"), "compat"))
    {
        return uri.getPath();
    }
    Poco::URI newUri(uri);
    const std::string key = newUri.getAuthority();
    if (Util::matchRegex(AliasHosts, key))
    {
        newUri.setAuthority(AliasHosts[key]);
    }

    if (newUri.getAuthority().empty())
    {
        return newUri.getPath();
    }
    return newUri.getScheme() + "://" + newUri.getHost() + ':' + std::to_string(newUri.getPort()) +
           newUri.getPath();
}
#endif // !defined(BUILDING_TESTS)

#endif

#if !defined(BUILDING_TESTS)

void StorageBase::initialize()
{
#if !MOBILEAPP
    const auto& app = Poco::Util::Application::instance();
    FilesystemEnabled = app.config().getBool("storage.filesystem[@allow]", false);

    parseWopiHost(app.config());

    parseAliases(app.config());

#ifdef ENABLE_FEATURE_LOCK
    CommandControl::LockManager::parseLockedHost(app.config());
#endif

#if ENABLE_SSL
    // FIXME: should use our own SSL socket implementation here.
    Poco::Crypto::initializeCrypto();
    Poco::Net::initializeSSL();

    // Init client
    Poco::Net::Context::Params sslClientParams;

    // false default for upgrade to preserve legacy configuration
    // in-config-file defaults are true.
    SSLAsScheme = COOLWSD::getConfigValue<bool>("storage.ssl.as_scheme", false);

    // Fallback to ssl.enable if not set - for back compatibility & simplicity.
    SSLEnabled = COOLWSD::getConfigValue<bool>(
        "storage.ssl.enable", COOLWSD::getConfigValue<bool>("ssl.enable", true));

#if ENABLE_DEBUG
    char *StorageSSLEnabled = getenv("STORAGE_SSL_ENABLE");
    if (StorageSSLEnabled != NULL)
    {
        if (!strcasecmp(StorageSSLEnabled, "true"))
            SSLEnabled = true;
        else if (!strcasecmp(StorageSSLEnabled, "false"))
            SSLEnabled = false;
    }
#endif

    if (SSLEnabled)
    {
        sslClientParams.certificateFile = COOLWSD::getPathFromConfigWithFallback("storage.ssl.cert_file_path", "ssl.cert_file_path");
        sslClientParams.privateKeyFile = COOLWSD::getPathFromConfigWithFallback("storage.ssl.key_file_path", "ssl.key_file_path");
        sslClientParams.caLocation = COOLWSD::getPathFromConfigWithFallback("storage.ssl.ca_file_path", "ssl.ca_file_path");
        sslClientParams.cipherList = COOLWSD::getPathFromConfigWithFallback("storage.ssl.cipher_list", "ssl.cipher_list");

        sslClientParams.verificationMode = (sslClientParams.caLocation.empty() ? Poco::Net::Context::VERIFY_NONE : Poco::Net::Context::VERIFY_STRICT);
        sslClientParams.loadDefaultCAs = true;
    }
    else
        sslClientParams.verificationMode = Poco::Net::Context::VERIFY_NONE;

    Poco::SharedPtr<Poco::Net::PrivateKeyPassphraseHandler> consoleClientHandler = new Poco::Net::KeyConsoleHandler(false);
    Poco::SharedPtr<Poco::Net::InvalidCertificateHandler> invalidClientCertHandler = new Poco::Net::AcceptCertificateHandler(false);

    Poco::Net::Context::Ptr sslClientContext = new Poco::Net::Context(Poco::Net::Context::CLIENT_USE, sslClientParams);
    sslClientContext->disableProtocols(Poco::Net::Context::Protocols::PROTO_SSLV2 |
                                       Poco::Net::Context::Protocols::PROTO_SSLV3 |
                                       Poco::Net::Context::Protocols::PROTO_TLSV1);
    Poco::Net::SSLManager::instance().initializeClient(consoleClientHandler, invalidClientCertHandler, sslClientContext);

    // Initialize our client SSL context.
    ssl::Manager::initializeClientContext(
        sslClientParams.certificateFile, sslClientParams.privateKeyFile, sslClientParams.caLocation,
        sslClientParams.cipherList,
        sslClientParams.caLocation.empty() ? ssl::CertificateVerification::Disabled
                                           : ssl::CertificateVerification::Required);
    if (!ssl::Manager::isClientContextInitialized())
        LOG_ERR("Failed to initialize Client SSL.");
    else
        LOG_INF("Initialized Client SSL.");
#endif
#else
    FilesystemEnabled = true;
#endif
}

bool StorageBase::allowedWopiHost(const std::string& host)
{
    return WopiEnabled && WopiHosts.match(host);
}

bool StorageBase::allowedAlias(const Poco::URI& uri)
{
    if (Util::iequal(config::getString("storage.wopi.alias_groups[@mode]", "first"), "compat"))
    {
        return true;
    }

    if (AllHosts.empty())
    {
        if (FirstHost.empty())
        {
            FirstHost = uri.getAuthority();
        }
        else if (FirstHost != uri.getAuthority())
        {
            LOG_ERR("Only allowed host is: " << FirstHost);
            return false;
        }
    }
    else if (!Util::matchRegex(AllHosts, uri.getAuthority()))
    {
        LOG_ERR("Host: " << uri.getAuthority()
                         << " is not allowed, It is not part of alias_groups configuration");
        return false;
    }
    return true;
}

#if !MOBILEAPP

bool isLocalhost(const std::string& targetHost)
{
    const std::string targetAddress = net::resolveHostAddress(targetHost);

    if (net::isLocalhost(targetHost))
    {
        LOG_INF("WOPI host [" << targetHost << "] is on the same host as the WOPI client: \""
                              << targetAddress << "\". Connection is allowed.");
        return true;
    }

    LOG_INF("WOPI host [" << targetHost << "] is not on the same host as the WOPI client: \""
                          << targetAddress << "\". Connection is not allowed.");
    return false;
}

#endif

bool isTemplate(const std::string& filename)
{
    std::vector<std::string> templateExtensions {".stw", ".ott", ".dot", ".dotx", ".dotm", ".otm", ".stc", ".ots", ".xltx", ".xltm", ".sti", ".otp", ".potx", ".potm", ".std", ".otg"};
    for (auto & extension : templateExtensions)
        if (Util::endsWith(filename, extension))
            return true;
    return false;
}

std::unique_ptr<StorageBase> StorageBase::create(const Poco::URI& uri, const std::string& jailRoot,
                                                 const std::string& jailPath, bool takeOwnership)
{
    // FIXME: By the time this gets called we have already sent to the client three
    // 'statusindicator:' messages: 'find', 'connect' and 'ready'. We should ideally do the checks
    // here much earlier. Also, using exceptions is lame and makes understanding the code harder,
    // but that is just my personal preference.

    std::unique_ptr<StorageBase> storage;

    if (UnitWSD::get().createStorage(uri, jailRoot, jailPath, storage))
    {
        LOG_INF("Storage create hooked.");
        if (storage)
        {
            return storage;
        }
    }
    else if (uri.isRelative() || uri.getScheme() == "file")
    {
        LOG_INF("Public URI [" << COOLWSD::anonymizeUrl(uri.toString()) << "] is a file.");

#if ENABLE_DEBUG
        if (std::getenv("FAKE_UNAUTHORIZED"))
        {
            LOG_FTL("Faking an UnauthorizedRequestException");
            throw UnauthorizedRequestException("No acceptable WOPI hosts found matching the target host in config.");
        }
#endif
        if (FilesystemEnabled || takeOwnership)
        {
            return std::unique_ptr<StorageBase>(
                new LocalStorage(uri, jailRoot, jailPath, takeOwnership));
        }

        LOG_ERR("Local Storage is disabled by default. Enable in the config file or on the command-line to enable.");
    }
#if !MOBILEAPP
    else if (WopiEnabled)
    {
        LOG_INF("Public URI [" << COOLWSD::anonymizeUrl(uri.toString()) << "] considered WOPI.");
        const auto& targetHost = uri.getHost();
        bool allowed(false);
        if ((StorageBase::allowedWopiHost(targetHost) && StorageBase::allowedAlias(uri)) ||
            isLocalhost(targetHost))
        {
            allowed = true;
        }
        if (!allowed)
        {
            // check if the IP address is in the list of allowed hosts
            const auto hostAddresses(Poco::Net::DNS::resolve(targetHost));
            for (auto &address : hostAddresses.addresses())
            {
                if (StorageBase::allowedWopiHost(address.toString()) &&
                    StorageBase::allowedAlias(uri))
                {
                    allowed = true;
                    break;
                }
            }
        }
        if (allowed)
            return std::unique_ptr<StorageBase>(new WopiStorage(uri, jailRoot, jailPath));
        LOG_ERR("No acceptable WOPI hosts found matching the target host [" << targetHost << "] in config.");
        throw UnauthorizedRequestException("No acceptable WOPI hosts found matching the target host [" + targetHost + "] in config.");
    }
#endif
    throw BadRequestException("No Storage configured or invalid URI.");
}

std::atomic<unsigned> LocalStorage::LastLocalStorageId;

std::unique_ptr<LocalStorage::LocalFileInfo> LocalStorage::getLocalFileInfo()
{
    const Poco::Path path = getUri().getPath();
    LOG_DBG("Getting info for local uri [" << COOLWSD::anonymizeUrl(getUri().toString()) << "], path [" << COOLWSD::anonymizeUrl(path.toString()) << "].");

    const FileUtil::Stat stat(path.toString());
    const std::chrono::system_clock::time_point lastModified = stat.modifiedTimepoint();

    setFileInfo(FileInfo(path.getFileName(), "LocalOwner",
                         Util::getIso8601FracformatTime(lastModified)));

    // Set automatic userid and username.
    const std::string userId = std::to_string(LastLocalStorageId++);
    std::string userNameString;

#if MOBILEAPP
    if (user_name != nullptr)
        userNameString = std::string(user_name);
#endif
    if (userNameString.size() == 0)
        userNameString = "LocalUser#" + userId;

    return std::unique_ptr<LocalStorage::LocalFileInfo>(
        new LocalFileInfo({"LocalUser" + userId, userNameString}));
}

std::string LocalStorage::downloadStorageFileToLocal(const Authorization& /*auth*/,
                                                     LockContext& /*lockCtx*/,
                                                     const std::string& /*templateUri*/)
{
#if !MOBILEAPP
    // /chroot/jailId/user/doc/childId/file.ext
    const std::string filename = Poco::Path(getUri().getPath()).getFileName();
    setRootFilePath(Poco::Path(getLocalRootPath(), filename).toString());
    setRootFilePathAnonym(COOLWSD::anonymizeUrl(getRootFilePath()));
    LOG_INF("Public URI [" << COOLWSD::anonymizeUrl(getUri().getPath()) <<
            "] jailed to [" << getRootFilePathAnonym() << "].");

    // Despite the talk about URIs it seems that _uri is actually just a pathname here
    const std::string publicFilePath = getUri().getPath();
    if (!Poco::File(publicFilePath).exists())
    {
        LOG_ERR("Local file URI [" << publicFilePath << "] invalid or doesn't exist.");
        throw BadRequestException("Invalid URI: " + getUri().toString());
    }

    if (!FileUtil::checkDiskSpace(getRootFilePath()))
    {
        throw StorageSpaceLowException("Low disk space for " + getRootFilePathAnonym());
    }

    if (_isTemporaryFile)
    {
        try
        {
            // Neither link nor copy, just move, it's a temporary file.
            Poco::File(publicFilePath).moveTo(getRootFilePath());

            // Cleanup the directory after moving.
            const std::string dir = Poco::Path(publicFilePath).parent().toString();
            if (FileUtil::isEmptyDirectory(dir))
                FileUtil::removeFile(dir);
        }
        catch (const Poco::Exception& exc)
        {
            LOG_ERR("Failed to move [" << COOLWSD::anonymizeUrl(publicFilePath) << "] to ["
                                       << getRootFilePathAnonym() << "]: " << exc.displayText());
        }
    }

    if (!FileUtil::Stat(getRootFilePath()).exists())
    {
        // Try to link.
        LOG_INF("Linking " << COOLWSD::anonymizeUrl(publicFilePath) << " to "
                           << getRootFilePathAnonym());
        if (!Poco::File(getRootFilePath()).exists()
            && link(publicFilePath.c_str(), getRootFilePath().c_str()) == -1)
        {
            // Failed
            LOG_INF("link(\"" << COOLWSD::anonymizeUrl(publicFilePath) << "\", \""
                              << getRootFilePathAnonym() << "\") failed. Will copy. Linking error: "
                              << Util::symbolicErrno(errno) << ' ' << strerror(errno));
        }
    }

    try
    {
        // Fallback to copying.
        if (!FileUtil::Stat(getRootFilePath()).exists())
        {
            FileUtil::copyFileTo(publicFilePath, getRootFilePath());
            _isCopy = true;
        }
    }
    catch (const Poco::Exception& exc)
    {
        LOG_ERR("copyTo(\"" << COOLWSD::anonymizeUrl(publicFilePath) << "\", \""
                            << getRootFilePathAnonym() << "\") failed: " << exc.displayText());
        throw;
    }

    setDownloaded(true);

    // Now return the jailed path.
#ifndef KIT_IN_PROCESS
    if (COOLWSD::NoCapsForKit)
        return getRootFilePath();
    else
        return Poco::Path(getJailPath(), filename).toString();
#else
    return getRootFilePath();
#endif

#else // MOBILEAPP

    // In the mobile app we use no jail
    setRootFilePath(getUri().getPath());

    return getRootFilePath();
#endif
}

StorageBase::UploadResult
LocalStorage::uploadLocalFileToStorage(const Authorization& /*auth*/, LockContext& /*lockCtx*/,
                                       const std::string& /*saveAsPath*/,
                                       const std::string& /*saveAsFilename*/, bool /*isRename*/)
{
    const std::string path = getUri().getPath();

    try
    {
        LOG_TRC("Copying local file to local file storage (isCopy: " << _isCopy << ") for "
                                                                     << getRootFilePathAnonym());

        // Copy the file back.
        if (_isCopy && Poco::File(getRootFilePathUploading()).exists())
            FileUtil::copyFileTo(getRootFilePathUploading(), path);

        // update its fileinfo object. This is used later to check if someone else changed the
        // document while we are/were editing it
        getFileInfo().setLastModifiedTime(
            Util::getIso8601FracformatTime(FileUtil::Stat(path).modifiedTimepoint()));
        LOG_TRC("New FileInfo modified time in storage " << getFileInfo().getLastModifiedTime());
    }
    catch (const Poco::Exception& exc)
    {
        LOG_ERR("copyTo(\"" << getRootFilePathAnonym() << "\", \"" << COOLWSD::anonymizeUrl(path)
                            << "\") failed: " << exc.displayText());
        return UploadResult(UploadResult::Result::FAILED, "Internal error.");
    }

    return UploadResult(UploadResult::Result::OK);
}

#if !MOBILEAPP

Poco::Net::HTTPClientSession* StorageBase::getHTTPClientSession(const Poco::URI& uri)
{
    bool useSSL = false;
    if (SSLAsScheme)
    {
        // the WOPI URI itself should control whether we use SSL or not
        // for whether we verify vs. certificates, cf. above
        useSSL = uri.getScheme() != "http";
    }
    else
    {
        // We decoupled the Wopi communication from client communication because
        // the Wopi communication must have an independent policy.
        // So, we will use here only Storage settings.
        useSSL = SSLEnabled || COOLWSD::isSSLTermination();
    }
    // We decoupled the Wopi communication from client communication because
    // the Wopi communication must have an independent policy.
    // So, we will use here only Storage settings.
    Poco::Net::HTTPClientSession* session = useSSL
        ? new Poco::Net::HTTPSClientSession(uri.getHost(), uri.getPort(),
                                            Poco::Net::SSLManager::instance().defaultClientContext())
        : new Poco::Net::HTTPClientSession(uri.getHost(), uri.getPort());

    // Set the timeout to the configured value.
    static int timeoutSec = COOLWSD::getConfigValue<int>("net.connection_timeout_secs", 30);
    session->setTimeout(Poco::Timespan(timeoutSec, 0));

    return session;
}

std::shared_ptr<http::Session> StorageBase::getHttpSession(const Poco::URI& uri)
{
    bool useSSL = false;
    if (SSLAsScheme)
    {
        // the WOPI URI itself should control whether we use SSL or not
        // for whether we verify vs. certificates, cf. above
        useSSL = uri.getScheme() != "http";
    }
    else
    {
        // We decoupled the Wopi communication from client communication because
        // the Wopi communication must have an independent policy.
        // So, we will use here only Storage settings.
        useSSL = SSLEnabled || COOLWSD::isSSLTermination();
    }

    const auto protocol
        = useSSL ? http::Session::Protocol::HttpSsl : http::Session::Protocol::HttpUnencrypted;

    // Create the session.
    auto httpSession = http::Session::create(uri.getHost(), protocol, uri.getPort());

    static int timeoutSec = COOLWSD::getConfigValue<int>("net.connection_timeout_secs", 30);
    httpSession->setTimeout(std::chrono::seconds(timeoutSec));

    return httpSession;
}

namespace
{

static void addStorageDebugCookie(Poco::Net::HTTPRequest& request)
{
    (void) request;
#if ENABLE_DEBUG
    if (std::getenv("COOL_STORAGE_COOKIE"))
    {
        Poco::Net::NameValueCollection nvcCookies;
        StringVector cookieTokens = Util::tokenize(std::string(std::getenv("COOL_STORAGE_COOKIE")), ':');
        if (cookieTokens.size() == 2)
        {
            nvcCookies.add(cookieTokens[0], cookieTokens[1]);
            request.setCookies(nvcCookies);
            LOG_TRC("Added storage debug cookie [" << cookieTokens[0] << '=' << cookieTokens[1] << "].");
        }
    }
#endif
}

// access_token must be decoded
void addWopiProof(Poco::Net::HTTPRequest& request, const Poco::URI& uri,
                  const std::string& access_token)
{
    assert(!uri.isRelative());
    for (const auto& header : GetProofHeaders(access_token, uri.toString()))
        request.set(header.first, header.second);
}

std::map<std::string, std::string> GetQueryParams(const Poco::URI& uri)
{
    std::map<std::string, std::string> result;
    for (const auto& param : uri.getQueryParameters())
        result.emplace(param);
    return result;
}

} // anonymous namespace

#endif // !MOBILEAPP

void LockContext::initSupportsLocks()
{
#if MOBILEAPP
    _supportsLocks = false;
#else
    if (_supportsLocks)
        return;

    // first time token setup
    _supportsLocks = true;
    _lockToken = "cool-lock" + Util::rng::getHexString(8);
#endif
}

bool LockContext::needsRefresh(const std::chrono::steady_clock::time_point &now) const
{
    static int refreshSeconds = COOLWSD::getConfigValue<int>("storage.wopi.locking.refresh", 900);
    return _supportsLocks && _isLocked && refreshSeconds > 0 &&
        std::chrono::duration_cast<std::chrono::seconds>
        (now - _lastLockTime).count() >= refreshSeconds;
}

void LockContext::dumpState(std::ostream& os) const
{
    if (!_supportsLocks)
        return;

    os << "\n  LockContext:";
    os << "\n    locked: " << _isLocked;
    os << "\n    token: " << _lockToken;
    os << "\n    last locked: " << Util::getSteadyClockAsString(_lastLockTime);
}

#if !MOBILEAPP

void WopiStorage::initHttpRequest(Poco::Net::HTTPRequest& request, const Poco::URI& uri,
                                  const Authorization& auth) const
{
    request.set("User-Agent", WOPI_AGENT_STRING);

    auth.authorizeRequest(request);

    addStorageDebugCookie(request);

    // TODO: Avoid repeated parsing.
    std::map<std::string, std::string> params = GetQueryParams(uri);
    const auto it = params.find("access_token");
    if (it != params.end())
        addWopiProof(request, uri, it->second);

    // Helps wrt. debugging cluster cases from the logs
    request.set("X-COOL-WOPI-ServerId", Util::getProcessIdentifier());
}

http::Request WopiStorage::initHttpRequest(const Poco::URI& uri, const Authorization& auth) const
{
    http::Request httpRequest(uri.getPathAndQuery());

    //FIXME: Hack Hack Hack! Use own version.
    Poco::Net::HTTPRequest request;
    initHttpRequest(request, uri, auth);

    // Copy the headers, including the cookies.
    for (const auto& pair : request)
    {
        httpRequest.header().set(pair.first, pair.second);
    }

    return httpRequest;
}

std::unique_ptr<WopiStorage::WOPIFileInfo>
WopiStorage::getWOPIFileInfoForUri(Poco::URI uriObject, const Authorization& auth,
                                   LockContext& lockCtx, unsigned redirectLimit)
{
    ProfileZone profileZone("WopiStorage::getWOPIFileInfo", { {"url", _fileUrl} });

    // update the access_token to the one matching to the session
    auth.authorizeURI(uriObject);
    const std::string uriAnonym = COOLWSD::anonymizeUrl(uriObject.toString());

    LOG_DBG("Getting info for wopi uri [" << uriAnonym << "].");

    std::string wopiResponse;
    std::chrono::milliseconds callDurationMs;
    try
    {
        std::shared_ptr<http::Session> httpSession = getHttpSession(uriObject);
        http::Request httpRequest = initHttpRequest(uriObject, auth);

        const auto startTime = std::chrono::steady_clock::now();

        Log::StreamLogger logger = Log::trace();
        if (logger.enabled())
        {
            logger << "WOPI::CheckFileInfo request header for URI [" << uriAnonym << "]:\n";
            for (const auto& pair : httpRequest.header())
            {
                logger << '\t' << pair.first << ": " << pair.second << " / ";
            }

            LOG_END(logger, true);
        }

        const std::shared_ptr<const http::Response> httpResponse
            = httpSession->syncRequest(httpRequest);

        callDurationMs = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - startTime);

        if (httpResponse->statusLine().statusCode() == Poco::Net::HTTPResponse::HTTP_FOUND ||
            httpResponse->statusLine().statusCode() == Poco::Net::HTTPResponse::HTTP_MOVED_PERMANENTLY ||
            httpResponse->statusLine().statusCode() == Poco::Net::HTTPResponse::HTTP_TEMPORARY_REDIRECT ||
            httpResponse->statusLine().statusCode() == Poco::Net::HTTPResponse::HTTP_PERMANENT_REDIRECT)
        {
            if (redirectLimit)
            {
                const std::string& location = httpResponse->get("Location");
                LOG_TRC("WOPI::CheckFileInfo redirect to URI [" << COOLWSD::anonymizeUrl(location) << "]");

                Poco::URI redirectUriObject(location);
                setUri(redirectUriObject);
                return getWOPIFileInfoForUri(redirectUriObject, auth, lockCtx, redirectLimit - 1);
            }
            else
            {
                LOG_WRN("WOPI::CheckFileInfo redirected too many times - URI [" << uriAnonym << "]");
            }
        }

        // Note: we don't log the response if obfuscation is enabled, except for failures.
        wopiResponse = httpResponse->getBody();
        const bool failed
            = (httpResponse->statusLine().statusCode() != Poco::Net::HTTPResponse::HTTP_OK);

        Log::StreamLogger logRes = failed ? Log::error() : Log::trace();
        if (logRes.enabled())
        {
            logRes << "WOPI::CheckFileInfo " << (failed ? "failed" : "returned") << " for URI ["
                   << uriAnonym << "]: " << httpResponse->statusLine().statusCode() << ' '
                   << httpResponse->statusLine().reasonPhrase() << ". Headers: ";
            for (const auto& pair : httpResponse->header())
            {
                logRes << '\t' << pair.first << ": " << pair.second << " / ";
            }

            if (failed)
                logRes << "\tBody: [" << wopiResponse << "]";

            LOG_END(logRes, true);
        }

        if (failed)
        {
            if (httpResponse->statusLine().statusCode() == Poco::Net::HTTPResponse::HTTP_FORBIDDEN)
                throw UnauthorizedRequestException(
                    "Access denied, 403. WOPI::CheckFileInfo failed on: " + uriAnonym);

            throw StorageConnectionException("WOPI::CheckFileInfo failed: " + wopiResponse);
        }
    }
    catch (const Poco::Exception& pexc)
    {
        LOG_ERR("Cannot get file info from WOPI storage uri [" << uriAnonym << "]. Error: " <<
                pexc.displayText() << (pexc.nested() ? " (" + pexc.nested()->displayText() + ')' : ""));
        throw;
    }
    catch (const BadRequestException& exc)
    {
        LOG_ERR("Cannot get file info from WOPI storage uri [" << uriAnonym << "]. Error: " << exc.what());
    }

    Poco::JSON::Object::Ptr object;
    if (JsonUtil::parseJSON(wopiResponse, object))
    {
        if (COOLWSD::AnonymizeUserData)
            LOG_DBG("WOPI::CheckFileInfo (" << callDurationMs << "): anonymizing...");
        else
            LOG_DBG("WOPI::CheckFileInfo (" << callDurationMs << "): " << wopiResponse);

        std::size_t size = 0;
        std::string filename, ownerId, lastModifiedTime;

        JsonUtil::findJSONValue(object, "Size", size);
        JsonUtil::findJSONValue(object, "OwnerId", ownerId);
        JsonUtil::findJSONValue(object, "BaseFileName", filename);
        JsonUtil::findJSONValue(object, "LastModifiedTime", lastModifiedTime);

        FileInfo fileInfo = FileInfo({filename, ownerId, lastModifiedTime});
        setFileInfo(fileInfo);

        if (COOLWSD::AnonymizeUserData)
            Util::mapAnonymized(Util::getFilenameFromURL(filename), Util::getFilenameFromURL(getUri().toString()));

        auto wopiInfo = Util::make_unique<WopiStorage::WOPIFileInfo>(fileInfo, callDurationMs, object, uriObject);
        if (wopiInfo->getSupportsLocks())
            lockCtx.initSupportsLocks();

        // If FileUrl is set, we use it for GetFile.
        _fileUrl = wopiInfo->getFileUrl();

        return wopiInfo;
    }
    else
    {
        if (COOLWSD::AnonymizeUserData)
            wopiResponse = "obfuscated";

        LOG_ERR("WOPI::CheckFileInfo ("
                << callDurationMs
                << ") failed or no valid JSON payload returned. Access denied. Original response: ["
                << wopiResponse << "].");

        throw UnauthorizedRequestException("Access denied. WOPI::CheckFileInfo failed on: " + uriAnonym);
    }
}

std::unique_ptr<WopiStorage::WOPIFileInfo> WopiStorage::getWOPIFileInfo(const Authorization& auth,
                                                                        LockContext& lockCtx)
{
    Poco::URI uriObject(getUri());
    return getWOPIFileInfoForUri(uriObject, auth, lockCtx, RedirectionLimit);
}

void WopiStorage::WOPIFileInfo::init()
{
    _userCanWrite = false;
    _enableOwnerTermination = false;
    _hidePrintOption = false;
    _hideSaveOption = false;
    _hideExportOption = false;
    _disablePrint = false;
    _disableExport = false;
    _disableCopy = false;
    _disableInactiveMessages = false;
    _downloadAsPostMessage = false;
    _userCanNotWriteRelative = true;
    _enableInsertRemoteImage = false;
    _enableShare = false;
    _supportsLocks = false;
    _supportsRename = false;
    _userCanRename = false;
    _hideUserList = "false";
    _disableChangeTrackingRecord = WOPIFileInfo::TriState::Unset;
    _disableChangeTrackingShow = WOPIFileInfo::TriState::Unset;
    _hideChangeTrackingControls = WOPIFileInfo::TriState::Unset;
}

WopiStorage::WOPIFileInfo::WOPIFileInfo(const FileInfo &fileInfo,
                                        std::chrono::milliseconds callDurationMs,
                                        Poco::JSON::Object::Ptr &object, Poco::URI &uriObject)
{
    init();

    const std::string &filename = fileInfo.getFilename();
    const std::string &ownerId = fileInfo.getOwnerId();

    JsonUtil::findJSONValue(object, "UserId", _userId);
    JsonUtil::findJSONValue(object, "UserFriendlyName", _username);
    JsonUtil::findJSONValue(object, "TemplateSaveAs", _templateSaveAs);
    JsonUtil::findJSONValue(object, "TemplateSource", _templateSource);

    // UserFriendlyName is used as the Author when loading the document.
    // If it's missing, document loading fails. Since the UserFriendlyName
    // field is optional in WOPI specs, it's often left out by integrators.
    if (_username.empty())
    {
        _username = "UnknownUser"; // Default to something sensible yet friendly.
        if (!_userId.empty())
            _username += '_' + _userId;

        LOG_ERR("WOPI::CheckFileInfo does not specify a valid UserFriendlyName for the current "
                "user. Temporarily ["
                << _username << "] will be used until a valid name is specified.");
    }

    std::ostringstream wopiResponse;

    // Anonymize key values.
    if (COOLWSD::AnonymizeUserData)
    {
        JsonUtil::findJSONValue(object, "ObfuscatedUserId", _obfuscatedUserId, false);
        if (!_obfuscatedUserId.empty())
        {
            Util::mapAnonymized(ownerId, _obfuscatedUserId);
            Util::mapAnonymized(_userId, _obfuscatedUserId);
            Util::mapAnonymized(_username, _obfuscatedUserId);
        }

        Poco::JSON::Object::Ptr anonObject(object);

        // Set anonymized version of the above fields before logging.
        // Note: anonymization caches the result, so we don't need to store here.
        if (COOLWSD::AnonymizeUserData)
            anonObject->set("BaseFileName", COOLWSD::anonymizeUrl(filename));

        // If obfuscatedUserId is provided, then don't log the originals and use it.
        if (COOLWSD::AnonymizeUserData && _obfuscatedUserId.empty())
        {
            anonObject->set("OwnerId", COOLWSD::anonymizeUsername(ownerId));
            anonObject->set("UserId", COOLWSD::anonymizeUsername(_userId));
            anonObject->set("UserFriendlyName", COOLWSD::anonymizeUsername(_username));
        }
        anonObject->stringify(wopiResponse);
    }
    else
        object->stringify(wopiResponse);

    LOG_DBG("WOPI::CheckFileInfo (" << callDurationMs << "): " << wopiResponse.str());

    JsonUtil::findJSONValue(object, "UserExtraInfo", _userExtraInfo);
    JsonUtil::findJSONValue(object, "WatermarkText", _watermarkText);
    JsonUtil::findJSONValue(object, "UserCanWrite", _userCanWrite);
    JsonUtil::findJSONValue(object, "PostMessageOrigin", _postMessageOrigin);
    JsonUtil::findJSONValue(object, "HidePrintOption", _hidePrintOption);
    JsonUtil::findJSONValue(object, "HideSaveOption", _hideSaveOption);
    JsonUtil::findJSONValue(object, "HideExportOption", _hideExportOption);
    JsonUtil::findJSONValue(object, "EnableOwnerTermination", _enableOwnerTermination);
    JsonUtil::findJSONValue(object, "DisablePrint", _disablePrint);
    JsonUtil::findJSONValue(object, "DisableExport", _disableExport);
    JsonUtil::findJSONValue(object, "DisableCopy", _disableCopy);
    JsonUtil::findJSONValue(object, "DisableInactiveMessages", _disableInactiveMessages);
    JsonUtil::findJSONValue(object, "DownloadAsPostMessage", _downloadAsPostMessage);
    JsonUtil::findJSONValue(object, "UserCanNotWriteRelative", _userCanNotWriteRelative);
    JsonUtil::findJSONValue(object, "EnableInsertRemoteImage", _enableInsertRemoteImage);
    JsonUtil::findJSONValue(object, "EnableShare", _enableShare);
    JsonUtil::findJSONValue(object, "HideUserList", _hideUserList);
    JsonUtil::findJSONValue(object, "SupportsLocks", _supportsLocks);
    JsonUtil::findJSONValue(object, "SupportsRename", _supportsRename);
    JsonUtil::findJSONValue(object, "UserCanRename", _userCanRename);
    JsonUtil::findJSONValue(object, "BreadcrumbDocName", _breadcrumbDocName);
    JsonUtil::findJSONValue(object, "FileUrl", _fileUrl);

#ifdef ENABLE_FEATURE_LOCK
    bool isUserLocked = false;
    JsonUtil::findJSONValue(object, "IsUserLocked", isUserLocked);

    if (config::getBool("feature_lock.locked_hosts[@allow]", false))
    {
        bool isReadOnly = false;
        isUserLocked = false;

        const std::string host = uriObject.getHost();

        if (CommandControl::LockManager::hostExist(host))
        {
            isReadOnly = CommandControl::LockManager::isHostReadOnly(host);
            isUserLocked = CommandControl::LockManager::isHostCommandDisabled(host);
        }
        else
        {
            LOG_INF("Could not find matching locked host so applying fallback settings");
            isReadOnly = config::getBool("feature_lock.locked_hosts.fallback[@read_only]", false);
            isUserLocked =
                config::getBool("feature_lock.locked_hosts.fallback[@disabled_commands]", false);
        }

        if (isReadOnly)
        {
            isUserLocked = true;
        }
        CommandControl::LockManager::setHostReadOnly(isReadOnly);
    }
    CommandControl::LockManager::setLockedUser(isUserLocked);
#else
    (void)uriObject;
#endif

    bool booleanFlag = false;
    JsonUtil::findJSONValue(object, "IsUserRestricted", booleanFlag);
    CommandControl::RestrictionManager::setRestrictedUser(booleanFlag);

    if (JsonUtil::findJSONValue(object, "DisableChangeTrackingRecord", booleanFlag))
        _disableChangeTrackingRecord = (booleanFlag ? WOPIFileInfo::TriState::True : WOPIFileInfo::TriState::False);
    if (JsonUtil::findJSONValue(object, "DisableChangeTrackingShow", booleanFlag))
        _disableChangeTrackingShow = (booleanFlag ? WOPIFileInfo::TriState::True : WOPIFileInfo::TriState::False);
    if (JsonUtil::findJSONValue(object, "HideChangeTrackingControls", booleanFlag))
        _hideChangeTrackingControls = (booleanFlag ? WOPIFileInfo::TriState::True : WOPIFileInfo::TriState::False);

    static const std::string overrideWatermarks
        = COOLWSD::getConfigValue<std::string>("watermark.text", "");
    if (!overrideWatermarks.empty())
        _watermarkText = overrideWatermarks;
    if (isTemplate(filename))
        _disableExport = true;
}

bool WopiStorage::updateLockState(const Authorization& auth, LockContext& lockCtx, bool lock)
{
    lockCtx._lockFailureReason.clear();
    if (!lockCtx._supportsLocks)
        return true;

    Poco::URI uriObject(getUri());
    auth.authorizeURI(uriObject);

    Poco::URI uriObjectAnonym(getUri());
    uriObjectAnonym.setPath(COOLWSD::anonymizeUrl(uriObjectAnonym.getPath()));
    const std::string uriAnonym = uriObjectAnonym.toString();

    const std::string wopiLog(lock ? "WOPI::Lock" : "WOPI::Unlock");
    LOG_DBG(wopiLog << " requesting: " << uriAnonym);

    try
    {
        std::unique_ptr<Poco::Net::HTTPClientSession> psession(getHTTPClientSession(uriObject));

        Poco::Net::HTTPRequest request(Poco::Net::HTTPRequest::HTTP_POST,
                                       uriObject.getPathAndQuery(),
                                       Poco::Net::HTTPMessage::HTTP_1_1);
        initHttpRequest(request, uriObject, auth);

        request.set("X-WOPI-Override", lock ? "LOCK" : "UNLOCK");
        request.set("X-WOPI-Lock", lockCtx._lockToken);
        if (!getExtendedData().empty())
        {
            request.set("X-COOL-WOPI-ExtendedData", getExtendedData());
            request.set("X-LOOL-WOPI-ExtendedData", getExtendedData());
        }

        // IIS requires content-length for POST requests: see https://forums.iis.net/t/1119456.aspx
        request.setContentLength(0);

        psession->sendRequest(request);
        Poco::Net::HTTPResponse response;
        std::istream& rs = psession->receiveResponse(response);

        std::ostringstream oss;
        Poco::StreamCopier::copyStream(rs, oss);
        std::string responseString = oss.str();

        LOG_INF(wopiLog << " response: " << responseString <<
                " status " << response.getStatus());

        if (response.getStatus() == Poco::Net::HTTPResponse::HTTP_OK)
        {
            lockCtx._isLocked = lock;
            lockCtx._lastLockTime = std::chrono::steady_clock::now();
            return true;
        }
        else
        {
            std::string sMoreInfo = response.get("X-WOPI-LockFailureReason", "");
            if (!sMoreInfo.empty())
            {
                lockCtx._lockFailureReason = sMoreInfo;
                sMoreInfo = ", failure reason: \"" + sMoreInfo + "\"";
            }
            LOG_ERR("Un-successful " << wopiLog << " with status " << response.getStatus() <<
                    sMoreInfo << " and response: " << responseString);
        }
    }
    catch (const Poco::Exception& pexc)
    {
        LOG_ERR("Cannot " << wopiLog << " uri [" << uriAnonym << "]. Error: " <<
                pexc.displayText() << (pexc.nested() ? " (" + pexc.nested()->displayText() + ')' : ""));
    }
    catch (const BadRequestException& exc)
    {
        LOG_ERR("Cannot " << wopiLog << " uri [" << uriAnonym << "]. Error: " << exc.what());
    }
    return false;
}

/// uri format: http://server/<...>/wopi*/files/<id>/content
std::string WopiStorage::downloadStorageFileToLocal(const Authorization& auth,
                                                    LockContext& /*lockCtx*/,
                                                    const std::string& templateUri)
{
    ProfileZone profileZone("WopiStorage::downloadStorageFileToLocal", { {"url", _fileUrl} });

    if (!templateUri.empty())
    {
        // Download the template file and load it normally.
        // The document will get saved once loading in Core is complete.
        const std::string templateUriAnonym = COOLWSD::anonymizeUrl(templateUri);
        try
        {
            LOG_INF("WOPI::GetFile template source: " << templateUriAnonym);
            return downloadDocument(Poco::URI(templateUri), templateUriAnonym, auth,
                                    RedirectionLimit);
        }
        catch (const std::exception& ex)
        {
            LOG_ERR("Could not download template from [" + templateUriAnonym + "]. Error: "
                    << ex.what());
            throw; // Bubble-up the exception.
        }

        return std::string();
    }

    // First try the FileUrl, if provided.
    if (!_fileUrl.empty())
    {
        const std::string fileUrlAnonym = COOLWSD::anonymizeUrl(_fileUrl);
        try
        {
            LOG_INF("WOPI::GetFile using FileUrl: " << fileUrlAnonym);
            return downloadDocument(Poco::URI(_fileUrl), fileUrlAnonym, auth, RedirectionLimit);
        }
        catch (const StorageSpaceLowException&)
        {
            throw; // Bubble-up the exception.
        }
        catch (const std::exception& ex)
        {
            LOG_ERR("Could not download document from WOPI FileUrl [" + fileUrlAnonym
                        + "]. Will use default URL. Error: "
                    << ex.what());
        }
    }

    // Try the default URL, we either don't have FileUrl, or it failed.
    // WOPI URI to download files ends in '/contents'.
    // Add it here to get the payload instead of file info.
    Poco::URI uriObject(getUri());
    uriObject.setPath(uriObject.getPath() + "/contents");
    auth.authorizeURI(uriObject);

    Poco::URI uriObjectAnonym(getUri());
    uriObjectAnonym.setPath(COOLWSD::anonymizeUrl(uriObjectAnonym.getPath()) + "/contents");
    const std::string uriAnonym = uriObjectAnonym.toString();

    try
    {
        LOG_INF("WOPI::GetFile using default URI: " << uriAnonym);
        return downloadDocument(uriObject, uriAnonym, auth, RedirectionLimit);
    }
    catch (const std::exception& ex)
    {
        LOG_ERR("Cannot download document from WOPI storage uri [" + uriAnonym + "]. Error: "
                << ex.what());
        throw; // Bubble-up the exception.
    }

    return std::string();
}

std::string WopiStorage::downloadDocument(const Poco::URI& uriObject, const std::string& uriAnonym,
                                          const Authorization& auth, unsigned redirectLimit)
{
    const auto startTime = std::chrono::steady_clock::now();
    std::shared_ptr<http::Session> httpSession = getHttpSession(uriObject);

    http::Request httpRequest = initHttpRequest(uriObject, auth);

    setRootFilePath(Poco::Path(getLocalRootPath(), getFileInfo().getFilename()).toString());
    setRootFilePathAnonym(COOLWSD::anonymizeUrl(getRootFilePath()));

    if (!FileUtil::checkDiskSpace(getRootFilePath()))
    {
        throw StorageSpaceLowException("Low disk space for " + getRootFilePathAnonym());
    }

    LOG_TRC("Downloading from [" << uriAnonym << "] to [" << getRootFilePath()
                                 << "]: " << httpRequest.header().toString());
    const std::shared_ptr<const http::Response> httpResponse
        = httpSession->syncDownload(httpRequest, getRootFilePath());

    const std::chrono::milliseconds diff = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - startTime);

    if (httpResponse->statusLine().statusCode() == Poco::Net::HTTPResponse::HTTP_OK)
    {
        // Log the response header.
        Log::StreamLogger logger = Log::trace();
        if (logger.enabled())
        {
            logger << "WOPI::GetFile response header for URI [" << uriAnonym << "]:\n";
            for (const auto& pair : httpResponse->header())
            {
                logger << '\t' << pair.first << ": " << pair.second << " / ";
            }

            LOG_END(logger, true);
        }
    }
    else if (httpResponse->statusLine().statusCode() == Poco::Net::HTTPResponse::HTTP_FOUND ||
            httpResponse->statusLine().statusCode() == Poco::Net::HTTPResponse::HTTP_MOVED_PERMANENTLY ||
            httpResponse->statusLine().statusCode() == Poco::Net::HTTPResponse::HTTP_TEMPORARY_REDIRECT ||
            httpResponse->statusLine().statusCode() == Poco::Net::HTTPResponse::HTTP_PERMANENT_REDIRECT)
    {
        if (redirectLimit)
        {
            const std::string& location = httpResponse->get("Location");
            LOG_TRC("WOPI::GetFile redirect to URI [" << COOLWSD::anonymizeUrl(location) << "]");

            Poco::URI redirectUriObject(location);
            return downloadDocument(redirectUriObject, uriAnonym, auth, redirectLimit - 1);
        }
        else
        {
            throw StorageConnectionException("WOPI::GetFile [" + uriAnonym
                                         + "] failed: redirected too many times");
        }
    }
    else
    {
        const std::string responseString = httpResponse->getBody();
        LOG_ERR("WOPI::GetFile [" << uriAnonym << "] failed with Status Code: "
                                  << httpResponse->statusLine().statusCode());
        throw StorageConnectionException("WOPI::GetFile [" + uriAnonym
                                         + "] failed: " + responseString);
    }

    // Successful
    const FileUtil::Stat fileStat(getRootFilePath());
    const std::size_t filesize = (fileStat.good() ? fileStat.size() : 0);
    LOG_INF("WOPI::GetFile downloaded " << filesize << " bytes from [" << uriAnonym << "] -> "
                                        << getRootFilePathAnonym() << " in " << diff);
    setDownloaded(true);

    // Now return the jailed path.
    if (COOLWSD::NoCapsForKit)
        return getRootFilePath();
    else
        return Poco::Path(getJailPath(), getFileInfo().getFilename()).toString();
}

/// A helper class to invoke the AsyncUploadCallback
/// when it exits its scope.
/// By default it invokes the callback with a failure state.
class ScopedInvokeAsyncUploadCallback
{
public:
    ScopedInvokeAsyncUploadCallback(StorageBase::AsyncUploadCallback asyncUploadCallback)
        : _asyncUploadCallback(std::move(asyncUploadCallback))
        , _arg(StorageBase::AsyncUpload(
              StorageBase::AsyncUpload::State::Error,
              StorageBase::UploadResult(StorageBase::UploadResult::Result::FAILED)))
    {
    }

    ~ScopedInvokeAsyncUploadCallback()
    {
        if (_asyncUploadCallback)
            _asyncUploadCallback(_arg);
    }

    /// Set a new callback argument.
    void setArg(StorageBase::AsyncUpload arg) { _arg = std::move(arg); }

private:
    StorageBase::AsyncUploadCallback _asyncUploadCallback;
    StorageBase::AsyncUpload _arg;
};

void WopiStorage::uploadLocalFileToStorageAsync(const Authorization& auth, LockContext& lockCtx,
                                                const std::string& saveAsPath,
                                                const std::string& saveAsFilename,
                                                const bool isRename, SocketPoll& socketPoll,
                                                const AsyncUploadCallback& asyncUploadCallback)
{
    ProfileZone profileZone("WopiStorage::uploadLocalFileToStorage", { {"url", _fileUrl} });

    // TODO: Check if this URI has write permission (canWrite = true)

    // Always invoke the callback with the result of the async upload.
    ScopedInvokeAsyncUploadCallback scopedInvokeCallback(asyncUploadCallback);

    //TODO: replace with state machine.
    if (_uploadHttpSession)
    {
        LOG_WRN("Upload is already in progress.");
        return;
    }

    const bool isSaveAs = !saveAsPath.empty() && !saveAsFilename.empty();
    const std::string filePath(isSaveAs ? saveAsPath : getRootFilePathUploading());
    const std::string filePathAnonym = COOLWSD::anonymizeUrl(filePath);

    const FileUtil::Stat fileStat(filePath);
    if (!fileStat.good())
    {
        LOG_ERR("Cannot access file [" << filePathAnonym << "] to upload to wopi storage.");
        scopedInvokeCallback.setArg(
            AsyncUpload(AsyncUpload::State::Error,
                        UploadResult(UploadResult::Result::FAILED, "File not found.")));
        return;
    }

    const std::size_t size = (fileStat.good() ? fileStat.size() : 0);

    Poco::URI uriObject(getUri());
    uriObject.setPath(isSaveAs || isRename ? uriObject.getPath()
                                           : uriObject.getPath() + "/contents");
    auth.authorizeURI(uriObject);

    const std::string uriAnonym = COOLWSD::anonymizeUrl(uriObject.toString());

    LOG_INF("Uploading " << size << " bytes from [" << filePathAnonym << "] to URI via WOPI ["
                         << uriAnonym << "].");

    const auto startTime = std::chrono::steady_clock::now();
    try
    {
        assert(!_uploadHttpSession && "Unexpected to have an upload http::session");
        _uploadHttpSession = getHttpSession(uriObject);

        http::Request httpRequest = initHttpRequest(uriObject, auth);
        httpRequest.setVerb(http::Request::VERB_POST);

        http::Header& httpHeader = httpRequest.header();

        // must include this header except for SaveAs
        if (!isSaveAs && lockCtx._supportsLocks)
            httpHeader.set("X-WOPI-Lock", lockCtx._lockToken);

        if (!isSaveAs && !isRename)
        {
            // normal save
            httpHeader.set("X-WOPI-Override", "PUT");
            httpHeader.set("X-COOL-WOPI-IsModifiedByUser", isUserModified() ? "true" : "false");
            httpHeader.set("X-LOOL-WOPI-IsModifiedByUser", isUserModified() ? "true" : "false");
            httpHeader.set("X-COOL-WOPI-IsAutosave", isAutosave() ? "true" : "false");
            httpHeader.set("X-LOOL-WOPI-IsAutosave", isAutosave() ? "true" : "false");
            httpHeader.set("X-COOL-WOPI-IsExitSave", isExitSave() ? "true" : "false");
            httpHeader.set("X-LOOL-WOPI-IsExitSave", isExitSave() ? "true" : "false");
            if (isExitSave())
                httpHeader.set("Connection", "close"); // Don't maintain the socket if we are exiting.
            if (!getExtendedData().empty())
            {
                httpHeader.set("X-COOL-WOPI-ExtendedData", getExtendedData());
                httpHeader.set("X-LOOL-WOPI-ExtendedData", getExtendedData());
            }

            if (!getForceSave())
            {
                // Request WOPI host to not overwrite if timestamps mismatch
                httpHeader.set("X-COOL-WOPI-Timestamp", getFileInfo().getLastModifiedTime());
                httpHeader.set("X-LOOL-WOPI-Timestamp", getFileInfo().getLastModifiedTime());
            }
        }
        else
        {
            // the suggested target has to be in UTF-7; default to extension
            // only when the conversion fails
            std::string suggestedTarget = '.' + Poco::Path(saveAsFilename).getExtension();

            //TODO: Perhaps we should cache this descriptor and reuse, as iconv_open might be expensive.
            const iconv_t cd = iconv_open("UTF-7", "UTF-8");
            if (cd == (iconv_t) -1)
                LOG_ERR("Failed to initialize iconv for UTF-7 conversion, using '" << suggestedTarget << "'.");
            else
            {
                std::vector<char> input(saveAsFilename.begin(), saveAsFilename.end());
                std::vector<char> buffer(8 * saveAsFilename.size());

                char* in = &input[0];
                std::size_t in_left = input.size();
                char* out = &buffer[0];
                std::size_t out_left = buffer.size();

                if (iconv(cd, &in, &in_left, &out, &out_left) == (size_t) -1)
                    LOG_ERR("Failed to convert '" << saveAsFilename << "' to UTF-7, using '" << suggestedTarget << "'.");
                else
                {
                    // conversion succeeded
                    suggestedTarget = std::string(&buffer[0], buffer.size() - out_left);
                    LOG_TRC("Converted '" << saveAsFilename << "' to UTF-7 as '" << suggestedTarget << "'.");
                }

                iconv_close(cd);
            }

            if (isRename)
            {
                // rename file
                httpHeader.set("X-WOPI-Override", "RENAME_FILE");
                httpHeader.set("X-WOPI-RequestedName", suggestedTarget);
            }
            else
            {
                // save as
                httpHeader.set("X-WOPI-Override", "PUT_RELATIVE");
                httpHeader.set("X-WOPI-Size", std::to_string(size));
                LOG_TRC("Save as: suggested target is '" << suggestedTarget << "'.");
                httpHeader.set("X-WOPI-SuggestedTarget", suggestedTarget);
            }
        }

        httpHeader.setContentType("application/octet-stream");
        httpHeader.setContentLength(size);

        httpRequest.setBodyFile(filePath);

        http::Session::FinishedCallback finishedCallback =
            [=](const std::shared_ptr<http::Session>& httpSession)
        {
            // Retire.
            _uploadHttpSession.reset();

            assert(httpSession && "Expected a valid http::Session");
            const std::shared_ptr<const http::Response> httpResponse = httpSession->response();

            _wopiSaveDuration = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - startTime);
            LOG_TRC("Finished async uploading in " << _wopiSaveDuration);

            WopiUploadDetails details = { filePathAnonym,
                                          uriAnonym,
                                          httpResponse->statusLine().reasonPhrase(),
                                          httpResponse->statusLine().statusCode(),
                                          size,
                                          isSaveAs,
                                          isRename };

            // Handle the response.
            const StorageBase::UploadResult res =
                handleUploadToStorageResponse(details, httpResponse->getBody());

            // Fire the callback to our client (DocBroker, typically).
            asyncUploadCallback(AsyncUpload(AsyncUpload::State::Complete, res));
        };

        _uploadHttpSession->setFinishedHandler(finishedCallback);

        LOG_DBG("Async upload request: " << httpRequest.header().toString());

        // Make the request.
        _uploadHttpSession->asyncRequest(httpRequest, socketPoll);

        scopedInvokeCallback.setArg(
            AsyncUpload(AsyncUpload::State::Running, UploadResult(UploadResult::Result::OK)));
        return;
    }
    catch (const Poco::Exception& ex)
    {
        LOG_ERR("Cannot upload file to WOPI storage uri ["
                << uriAnonym << "]. Error: " << ex.displayText()
                << (ex.nested() ? " (" + ex.nested()->displayText() + ')' : ""));
    }
    catch (const std::exception& ex)
    {
        LOG_ERR("Cannot upload file to WOPI storage uri [" + uriAnonym + "]. Error: " << ex.what());
    }

    scopedInvokeCallback.setArg(AsyncUpload(
        AsyncUpload::State::Error, UploadResult(UploadResult::Result::FAILED, "Internal error.")));
}

StorageBase::UploadResult WopiStorage::uploadLocalFileToStorage(const Authorization&, LockContext&,
                                                                const std::string&,
                                                                const std::string&, const bool)
{
    return UploadResult(UploadResult::Result::FAILED);
}

StorageBase::UploadResult
WopiStorage::handleUploadToStorageResponse(const WopiUploadDetails& details,
                                           std::string responseString)
{
    // Assume we failed, unless we have confirmation of success.
    StorageBase::UploadResult result(UploadResult::Result::FAILED, responseString);
    try
    {
        // Save a copy of the response because we might need to anonymize.
        const std::string origResponseString = responseString;

        const std::string wopiLog(details.isSaveAs
                                      ? "WOPI::PutRelativeFile"
                                      : (details.isRename ? "WOPI::RenameFile" : "WOPI::PutFile"));

        if (Log::infoEnabled())
        {
            if (COOLWSD::AnonymizeUserData)
            {
                Poco::JSON::Object::Ptr object;
                if (JsonUtil::parseJSON(responseString, object))
                {
                    // Anonymize the filename
                    std::string url;
                    std::string filename;
                    if (JsonUtil::findJSONValue(object, "Url", url)
                        && JsonUtil::findJSONValue(object, "Name", filename))
                    {
                        // Get the FileId form the URL, which we use as the anonymized filename.
                        std::string decodedUrl;
                        Poco::URI::decode(url, decodedUrl);
                        const std::string obfuscatedFileId = Util::getFilenameFromURL(decodedUrl);
                        Util::mapAnonymized(obfuscatedFileId,
                                            obfuscatedFileId); // Identity, to avoid re-anonymizing.

                        const std::string filenameOnly = Util::getFilenameFromURL(filename);
                        Util::mapAnonymized(filenameOnly, obfuscatedFileId);
                        object->set("Name", COOLWSD::anonymizeUrl(filename));
                    }

                    // Stringify to log.
                    std::ostringstream ossResponse;
                    object->stringify(ossResponse);
                    responseString = ossResponse.str();
                }
            }

            LOG_INF(wopiLog << " uploaded " << details.size << " bytes in " << _wopiSaveDuration
                            << " from [" << details.filePathAnonym << "] -> [" << details.uriAnonym
                            << "]: " << details.httpResponseCode << ' '
                            << details.httpResponseReason << ": " << responseString);
        }

        if (details.httpResponseCode == Poco::Net::HTTPResponse::HTTP_OK)
        {
            result.setResult(StorageBase::UploadResult::Result::OK);
            Poco::JSON::Object::Ptr object;
            if (JsonUtil::parseJSON(origResponseString, object))
            {
                const std::string lastModifiedTime
                    = JsonUtil::getJSONValue<std::string>(object, "LastModifiedTime");
                LOG_TRC(wopiLog << " returns LastModifiedTime [" << lastModifiedTime << "].");
                getFileInfo().setLastModifiedTime(lastModifiedTime);

                if (details.isSaveAs || details.isRename)
                {
                    const std::string name = JsonUtil::getJSONValue<std::string>(object, "Name");
                    LOG_TRC(wopiLog << " returns Name [" << COOLWSD::anonymizeUrl(name) << "].");

                    const std::string url = JsonUtil::getJSONValue<std::string>(object, "Url");
                    LOG_TRC(wopiLog << " returns Url [" << COOLWSD::anonymizeUrl(url) << "].");

                    result.setSaveAsResult(name, url);
                }
                // Reset the force save flag now, if any, since we are done saving
                // Next saves shouldn't be saved forcefully unless commanded
                forceSave(false);
            }
            else
            {
                LOG_ERR("Invalid or missing JSON in " << wopiLog << " HTTP_OK response.");
            }
        }
        else if (details.httpResponseCode == Poco::Net::HTTPResponse::HTTP_REQUEST_ENTITY_TOO_LARGE)
        {
            result.setResult(StorageBase::UploadResult::Result::DISKFULL);
        }
        else if (details.httpResponseCode == Poco::Net::HTTPResponse::HTTP_UNAUTHORIZED
                 || details.httpResponseCode == Poco::Net::HTTPResponse::HTTP_FORBIDDEN)
        {
            result.setResult(StorageBase::UploadResult::Result::UNAUTHORIZED);
        }
        else if (details.httpResponseCode == Poco::Net::HTTPResponse::HTTP_CONFLICT)
        {
            result.setResult(StorageBase::UploadResult::Result::CONFLICT);
            Poco::JSON::Object::Ptr object;
            if (JsonUtil::parseJSON(origResponseString, object))
            {
                const unsigned coolStatusCode
                    = JsonUtil::getJSONValue<unsigned>(object, "COOLStatusCode");
                const unsigned loolStatusCode
                    = JsonUtil::getJSONValue<unsigned>(object, "LOOLStatusCode");
                if (coolStatusCode == static_cast<unsigned>(COOLStatusCode::DOC_CHANGED) ||
                    loolStatusCode == static_cast<unsigned>(COOLStatusCode::DOC_CHANGED))
                {
                    result.setResult(StorageBase::UploadResult::Result::DOC_CHANGED);
                }
            }
            else
            {
                LOG_ERR("Invalid or missing JSON in " << wopiLog << " HTTP_CONFLICT response.");
            }
        }
        else
        {
            // Internal server error, and other failures.
            LOG_ERR("Unexpected response to "
                    << wopiLog << ". Cannot upload file to WOPI storage uri [" << details.uriAnonym
                    << "]: " << details.httpResponseCode << ' ' << details.httpResponseReason
                    << ": " << responseString);
            result.setResult(StorageBase::UploadResult::Result::FAILED);
        }
    }
    catch (const Poco::Exception& pexc)
    {
        LOG_ERR("Cannot upload file to WOPI storage uri ["
                << details.uriAnonym << "]. Error: " << pexc.displayText()
                << (pexc.nested() ? " (" + pexc.nested()->displayText() + ')' : ""));
        result.setResult(StorageBase::UploadResult::Result::FAILED);
    }
    catch (const BadRequestException& exc)
    {
        LOG_ERR("Cannot upload file to WOPI storage uri [" + details.uriAnonym + "]. Error: "
                << exc.what());
        result.setResult(StorageBase::UploadResult::Result::FAILED);
    }

    return result;
}

#endif // !MOBILEAPP

#endif // !defined(BUILDING_TESTS)
/* vim:set shiftwidth=4 softtabstop=4 expandtab: */
