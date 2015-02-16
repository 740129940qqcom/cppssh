/*
    cppssh - C++ ssh library
    Copyright (C) 2015  Chris Desjardins
    http://blog.chrisd.info cjd@chrisd.info

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

#include "transport.h"
#include "crypto.h"
#include "channel.h"
#include "packet.h"
#include "messages.h"
#include "x11channel.h"

#if defined(WIN32) || defined(__MINGW32__)
#   define SOCKET_BUFFER_TYPE char
#   define close closesocket
#   define SOCK_CAST (char*)
class WSockInitializer
{
public:
    WSockInitializer()
    {
        static WSADATA wsaData;
        WSAStartup(MAKEWORD(2, 2), &wsaData);
    }

    ~WSockInitializer()
    {
        WSACleanup();
    }
};

struct  sockaddr_un
{
    short sun_family;       /* AF_UNIX */
    char  sun_path[108];
};

WSockInitializer _wsock32_;
#else
#   define SOCKET_BUFFER_TYPE void
#   define SOCK_CAST (void*)
#   include <sys/socket.h>
#   include <netinet/in.h>
#   include <netdb.h>
#   include <unistd.h>
#   include <fcntl.h>
#   include <sys/un.h>
#endif

CppsshTransport::CppsshTransport(const std::shared_ptr<CppsshSession>& session)
    : CppsshBaseTransport(session)
{
}

CppsshTransport::~CppsshTransport()
{
    std::cout << "~CppsshTransport" << std::endl;
    _running = false;
    if (_rxThread.joinable() == true)
    {
        _rxThread.join();
    }
    if (_txThread.joinable() == true)
    {
        _txThread.join();
    }
}

bool CppsshBaseTransport::establish(const std::string& host, short port)
{
    bool ret = false;
    sockaddr_in remoteAddr;
    hostent* remoteHost;

    remoteHost = gethostbyname(host.c_str());
    if (!remoteHost || remoteHost->h_length == 0)
    {
        _session->_logger->pushMessage(std::stringstream() << "Host" << host << "not found.");
    }
    else
    {
        remoteAddr.sin_family = AF_INET;
        remoteAddr.sin_addr.s_addr = *(long*)remoteHost->h_addr_list[0];
        remoteAddr.sin_port = htons(port);

        _sock = socket(AF_INET, SOCK_STREAM, 0);
        if (_sock < 0)
        {
            _session->_logger->pushMessage("Failure to bind to socket.");
        }
        else
        {
            if (connect(_sock, (struct sockaddr*) &remoteAddr, sizeof(remoteAddr)) == -1)
            {
                _session->_logger->pushMessage(std::stringstream() << "Unable to connect to remote server: '" << host << "'.");
            }
            else
            {
                ret = setNonBlocking(true);
            }
        }
    }

    return ret;
}

bool CppsshBaseTransport::parseDisplay(const std::string& display, int* displayNum, int* screenNum)
{
    bool ret = false;
    size_t start = display.find(':') + 1;
    size_t mid = display.find('.');
    std::string dn(display.substr(start, mid - start));
    std::string sn(display.substr(mid + 1));
    if ((dn.length() > 0) && (sn.length() > 0))
    {
        std::istringstream dss(dn);
        dss >> *displayNum;

        std::istringstream sss(sn);
        sss >> *screenNum;
        ret = true;
    }
    return ret;
}

bool CppsshBaseTransport::establishX11()
{
    bool ret = false;
    std::string display;
    CppsshX11Channel::getDisplay(&display);

    if ((display.find("unix:") == 0) || (display.find(":") == 0) || (display.find("localhost:") == 0))
    {
        ret = establishLocalX11(display);
    }
    else
    {
        // FIXME: Connect to remote x11
    }
    return ret;
}
#ifdef WIN32
bool CppsshBaseTransport::establishLocalX11(const std::string& display)
{
    bool ret = false;

    _sock = socket(AF_INET, SOCK_STREAM, 0);
    if (_sock < 0)
    {
        _session->_logger->pushMessage(std::stringstream() << "Unable to open to X11 socket");
    }
    else
    {
        SOCKADDR_IN addr;
        memset(&addr, 0, sizeof(addr));

        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_ANY);
        addr.sin_port = htons((short)0);

        int bindRet = bind(_sock, (struct sockaddr *) &addr, sizeof(addr));
        if (bindRet == 0)
        {
            memset(&addr, 0, sizeof(addr));
            addr.sin_family = AF_INET;
            addr.sin_addr.s_addr = htonl(0x7f000001);
            addr.sin_port = htons((short)6000);
            int connectRet = connect(_sock, (struct sockaddr*)&addr, sizeof(addr));
            if (connectRet == 0)
            {
                // success
                ret = true;
                setNonBlocking(true);
            }
            else
            {
                _session->_logger->pushMessage(std::stringstream() << "Unable to connect to X11 socket " << WSAGetLastError());
                disconnect();
            }
        }
        else
        {
            _session->_logger->pushMessage(std::stringstream() << "Unable to bind to X11 socket " << strerror(errno));
            disconnect();
        }
    }
    return ret;
}
#else
bool CppsshBaseTransport::establishLocalX11(const std::string& display)
{
    bool ret = false;
    struct sockaddr_un addr;

    _sock = socket(AF_UNIX, SOCK_STREAM, 0);
    if (_sock < 0)
    {
        _session->_logger->pushMessage(std::stringstream() << "Unable to open to X11 socket");
    }
    else
    {
        int displayNum;
        int screenNum;
        parseDisplay(display, &displayNum, &screenNum);
        std::stringstream path;
        path << "/tmp/.X11-unix/X" << displayNum;

        memset(&addr, 0, sizeof(addr));
        addr.sun_family = AF_UNIX;
        strncpy(addr.sun_path, path.str().c_str(), sizeof(addr.sun_path));
        int connectRet = connect(_sock, (struct sockaddr*)&addr, sizeof(addr));
        if (connectRet == 0)
        {
            // success
            ret = true;
            setNonBlocking(true);
        }
        else
        {
            _session->_logger->pushMessage(std::stringstream() << "Unable to connect to X11 socket " << path.str() << " " << strerror(errno));
            disconnect();
        }
    }
    return ret;
}
#endif
void CppsshBaseTransport::disconnect()
{
    _running = false;
    close(_sock);
}

bool CppsshTransport::start()
{
    _rxThread = std::thread(&CppsshTransport::rxThread, this);
    _txThread = std::thread(&CppsshTransport::txThread, this);
    return true;
}

bool CppsshBaseTransport::setNonBlocking(bool on)
{
#if !defined(WIN32) && !defined(__MINGW32__)
    int options;
    if ((options = fcntl(_sock, F_GETFL)) < 0)
    {
        _session->_logger->pushMessage("Cannot read options of the socket.");
        return false;
    }

    if (on == true)
    {
        options = (options | O_NONBLOCK);
    }
    else
    {
        options = (options & ~O_NONBLOCK);
    }
    fcntl(_sock, F_SETFL, options);
#else
    unsigned long options = on;
    if (ioctlsocket(_sock, FIONBIO, &options))
    {
        _session->_logger->pushMessage("Cannot set asynch I/O on the socket.");
        return false;
    }
#endif
    return true;
}

void CppsshBaseTransport::setupFd(fd_set* fd)
{
#if defined(WIN32)
#pragma warning(push)
#pragma warning(disable : 4127)
#endif
    FD_ZERO(fd);
    FD_SET(_sock, fd);
#if defined(WIN32)
#pragma warning(pop)
#endif
}

bool CppsshBaseTransport::wait(bool isWrite)
{
    bool ret = false;
    int status = 0;
    struct timeval waitTime;
    waitTime.tv_sec = 0;
    waitTime.tv_usec = 0;
    std::chrono::steady_clock::time_point t0 = std::chrono::steady_clock::now();
    while ((_running == true) && (ret == false) && (std::chrono::steady_clock::now() < (t0 + std::chrono::milliseconds(_session->getTimeout()))))
    {
        fd_set fds;
        if (isWrite == false)
        {
            setupFd(&fds);
            status = select(_sock + 1, &fds, NULL, NULL, &waitTime);
        }
        else
        {
            setupFd(&fds);
            status = select(_sock + 1, NULL, &fds, NULL, &waitTime);
        }
        if ((status > 0) && (FD_ISSET(_sock, &fds)))
        {
            ret = true;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    return ret;
}

// Append new receive data to the end of the buffer
bool CppsshBaseTransport::receiveMessage(Botan::secure_vector<Botan::byte>* buffer)
{
    bool ret = true;
    int len = 0;
    int bufferLen = buffer->size();
    buffer->resize(CPPSSH_MAX_PACKET_LEN + bufferLen);

    if (wait(false) == true)
    {
        len = ::recv(_sock, (char*)buffer->data() + bufferLen, CPPSSH_MAX_PACKET_LEN, 0);
        if (len > 0)
        {
            bufferLen += len;
        }
    }
    buffer->resize(bufferLen);

    if ((_running == true) && (len < 0))
    {
        _session->_logger->pushMessage("Connection dropped.");
        _session->_channel->disconnect();
        ret = false;
    }

    return ret;
}

bool CppsshTransport::setupMessage(const Botan::secure_vector<Botan::byte>& buffer, Botan::secure_vector<Botan::byte>* outBuf)
{
    bool ret = true;
    size_t length = buffer.size();
    CppsshPacket out(outBuf);
    Botan::byte padLen;
    uint32_t packetLen;

    uint32_t cryptBlock = _session->_crypto->getEncryptBlock();
    if (cryptBlock == 0)
    {
        cryptBlock = 8;
    }

    padLen = (Botan::byte)(3 + cryptBlock - ((length + 8) % cryptBlock));
    packetLen = 1 + length + padLen;

    out.addInt(packetLen);
    out.addByte(padLen);
    out.addVector(buffer);

    Botan::secure_vector<Botan::byte> padBytes;
    padBytes.resize(padLen, 0);
    out.addVector(padBytes);
    return ret;
}

bool CppsshTransport::sendMessage(const Botan::secure_vector<Botan::byte>& buffer)
{
    bool ret;
    Botan::secure_vector<Botan::byte> buf;
    setupMessage(buffer, &buf);
    ret = CppsshBaseTransport::sendMessage(buf);
    return ret;
}

bool CppsshBaseTransport::sendMessage(const Botan::secure_vector<Botan::byte>& buffer)
{
    int len;
    size_t sent = 0;

    while ((sent < buffer.size()) && (_running == true))
    {
        if (wait(true) == true)
        {
            len = ::send(_sock, (char*)(buffer.data() + sent), buffer.size() - sent, 0);
        }
        else
        {
            break;
        }
        if ((_running == true) && (len < 0))
        {
            _session->_logger->pushMessage("Connection dropped.");
            _session->_channel->disconnect();
            break;
        }
        sent += len;
    }
    return sent == buffer.size();
}

void CppsshTransport::rxThread()
{
    std::cout << "starting rx thread" << std::endl;
    try
    {
        Botan::secure_vector<Botan::byte> incoming;
        size_t size = 0;
        while (_running == true)
        {
            if (incoming.size() < sizeof(uint32_t))
            {
                size = sizeof(uint32_t);
            }
            while ((incoming.size() < size) && (_running == true))
            {
                if (receiveMessage(&incoming) == false)
                {
                    return;
                }
                if (incoming.size() >= size)
                {
                    CppsshPacket packet(&incoming);
                    size = packet.getCryptoLength();
                }
            }
            if (incoming.empty() == false)
            {
                _session->_channel->handleReceived(incoming);
                if (incoming.size() == size)
                {
                    incoming.clear();
                }
                else
                {
                    incoming.erase(incoming.begin(), incoming.begin() + size);
                    CppsshPacket packet(&incoming);
                    size = packet.getCryptoLength();
                }
            }
        }
    }
    catch (const std::exception& ex)
    {
        _session->_logger->pushMessage(std::stringstream() << "rxThread exception: " << ex.what());
    }
    std::cout << "rx thread done" << std::endl;
}

void CppsshTransport::txThread()
{
    std::cout << "starting tx thread" << std::endl;
    try
    {
        while (_running == true)
        {
            if (_session->_channel->flushOutgoingChannelData() == false)
            {
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }
    catch (const std::exception& ex)
    {
        _session->_logger->pushMessage(std::stringstream() << "txThread exception: " << ex.what());
    }
    std::cout << "tx thread done" << std::endl;
}

CppsshBaseTransport::CppsshBaseTransport(const std::shared_ptr<CppsshSession>& session)
    : _session(session),
    _sock((SOCKET)-1),
    _running(true)
{
}

CppsshBaseTransport::~CppsshBaseTransport()
{
    _running = false;
}

