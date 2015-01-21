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
#include "packet.h"
#include <iostream>
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
}
;
WSockInitializer _wsock32_;
#else
#   define SOCKET_BUFFER_TYPE void
#   define SOCK_CAST (void*)
#   include <sys/socket.h>
#   include <netinet/in.h>
#   include <netdb.h>
#   include <unistd.h>
#   include <fcntl.h>
#endif

CppsshTransport::CppsshTransport(const std::shared_ptr<CppsshSession> &session, unsigned int timeout)
    : _session(session),
    _timeout(timeout),
    _txSeq(0),
    _rxSeq(0),
    _running(true)
{
}

CppsshTransport::~CppsshTransport()
{
    _running = false;
    if (_rxThread.joinable() == true)
    {
        _rxThread.join();
    }
}

int CppsshTransport::establish(const char* host, short port)
{
    sockaddr_in remoteAddr;
    hostent* remoteHost;

    remoteHost = gethostbyname(host);
    if (!remoteHost || remoteHost->h_length == 0)
    {
        _session->_logger->pushMessage(std::stringstream() << "Host" << host << "not found.");
        return -1;
    }
    remoteAddr.sin_family = AF_INET;
    remoteAddr.sin_addr.s_addr = *(long*) remoteHost->h_addr_list[0];
    remoteAddr.sin_port = htons(port);

    _sock = socket(AF_INET, SOCK_STREAM, 0);
    if (_sock < 0)
    {
        _session->_logger->pushMessage(std::stringstream() << "Failure to bind to socket.");
        return -1;
    }
    if (connect(_sock, (struct sockaddr*) &remoteAddr, sizeof(remoteAddr)) == -1)
    {
        _session->_logger->pushMessage(std::stringstream() << "Unable to connect to remote server: '" << host << "'.");
        return -1;
    }

    if (setNonBlocking(true) == false)
    {
        return -1;
    }

    return _sock;
}

bool CppsshTransport::start()
{
    _rxThread = std::thread(&CppsshTransport::rxThread, this);
    return true;
}

bool CppsshTransport::setNonBlocking(bool on)
{
#if !defined(WIN32) && !defined(__MINGW32__)
    int options;
    if ((options = fcntl(_sock, F_GETFL)) < 0)
    {
        _session->_logger->pushMessage(std::stringstream() << "Cannot read options of the socket.");
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
        _session->_logger->pushMessage(std::stringstream() << "Cannot set asynch I/O on the socket.");
        return false;
    }
#endif
    return true;
}

void CppsshTransport::setupFd(fd_set *fd)
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

bool CppsshTransport::wait(bool isWrite)
{
    int status;

    if (isWrite == false)
    {
        std::chrono::steady_clock::time_point t0 = std::chrono::steady_clock::now();
        while ((_running == true) && (std::chrono::steady_clock::now() < (t0 + std::chrono::seconds(_timeout))))
        {
            fd_set rfds;
            struct timeval waitTime;

            waitTime.tv_sec = 0;
            waitTime.tv_usec = 1000;
            setupFd(&rfds);
            status = select(_sock + 1, &rfds, NULL, NULL, &waitTime);
            if (status > 0)
            {
                break;
            }
        }
    }
    else
    {
        fd_set wfds;
        setupFd(&wfds);
        status = select(_sock + 1, NULL, &wfds, NULL, NULL);
    }

    return (status > 0) ? true : false;
}

bool CppsshTransport::receive(Botan::secure_vector<Botan::byte>* buffer)
{
    bool ret = true;
    int len = 0;
    buffer->resize(MAX_PACKET_LEN);

    if (wait(false) == true)
    {
        len = ::recv(_sock, (char*)buffer->data(), MAX_PACKET_LEN, 0);
        std::cout << "got: " << len << std::endl;
        if (len > 0)
        {
            buffer->resize(len);
        }
        else
        {
            buffer->clear();
        }
    }

    if (len == 0)
    {
        _session->_logger->pushMessage(std::stringstream() << "Received a packet of zero length.");
        ret = false;
    }

    if (len < 0)
    {
        _session->_logger->pushMessage(std::stringstream() << "Connection dropped.");
        ret = false;
    }

    return ret;
}

bool CppsshTransport::send(const Botan::secure_vector<Botan::byte>& buffer)
{
    int byteCount;
    size_t sent = 0;
    bool ret = true;
    while (sent < buffer.size())
    {
        if (wait(true) == true)
        {
            byteCount = ::send(_sock, (char*)(buffer.data() + sent), buffer.size() - sent, 0);
        }
        else
        {
            ret = false;
            break;
        }
        if (byteCount < 0)
        {
            ret = false;
            break;
        }
        sent += byteCount;
    }
    return ret;
}

bool CppsshTransport::sendPacket(const Botan::secure_vector<Botan::byte> &buffer)
{
    bool ret = true;
    size_t length = buffer.size();
    Botan::secure_vector<Botan::byte> buf;
    CppsshPacket out(&buf);
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
    out.addChar(padLen);
    out.addVector(buffer);

    Botan::secure_vector<Botan::byte> padBytes;
    padBytes.resize(padLen, 0);
    out.addVector(padBytes);

    if (_session->_crypto->isInited() == true)
    {
        Botan::secure_vector<Botan::byte> crypted;
        Botan::secure_vector<Botan::byte> hmac;
        if (_session->_crypto->encryptPacket(crypted, hmac, buf, _txSeq) == false)
        {
            _session->_logger->pushMessage(std::stringstream() << "Failure to encrypt the payload.");
            return false;
        }
        crypted += hmac;
        if (send(crypted) == false)
        {
            ret = false;
        }
    }
    else if (send(buf) == false)
    {
        ret = false;
    }
    if (ret == true)
    {
        _txSeq++;
    }
    return ret;
}

void CppsshTransport::rxThread()
{
    Botan::secure_vector<Botan::byte> decrypted;
    CppsshPacket packet(&_in);
    while (_running == true)
    {
        decrypted.clear();
        packet = &_in;
        uint32_t cryptoLen = 0;
        int macLen = 0;

        //if (bufferOnly == false)
        {
            size_t size = 4;
            if (_session->_crypto->isInited() == true)
            {
                size = _session->_crypto->getDecryptBlock();
            }
            while (_in.size() < size)
            {
                if (receive(&_in) == false)
                {
                    return;// -1;
                }
            }
        }
        if ((_session->_crypto->isInited() == true) && (_in.size() >= _session->_crypto->getDecryptBlock()))
        {
            _session->_crypto->decryptPacket(decrypted, _in, _session->_crypto->getDecryptBlock());
            packet = &decrypted;
            macLen = _session->_crypto->getMacInLen();
        }
        cryptoLen = packet.getCryptoLength();
        //if ((bufferOnly == false) || ((_session->_crypto->isInited() == true) && (packet.getCommand() > 0) && (packet.getCommand() < 0xff)))
        if ((_session->_crypto->isInited() == true) && (packet.getCommand() > 0) && (packet.getCommand() < 0xff))
        {
            while ((cryptoLen + macLen) > _in.size())
            {
                if (receive(&_in) == false)
                {
                    return; //-1;
                }
            }
        }

        if (_session->_crypto->isInited() == true)
        {
            if (cryptoLen > _session->_crypto->getDecryptBlock())
            {
                Botan::secure_vector<Botan::byte> tmpVar;
                tmpVar = Botan::secure_vector<Botan::byte>(_in.begin() + _session->_crypto->getDecryptBlock(), _in.begin() + cryptoLen);
                _session->_crypto->decryptPacket(tmpVar, tmpVar, tmpVar.size());
                decrypted += tmpVar;
            }
            if (_session->_crypto->getMacInLen() && (_in.size() > 0) && (_in.size() >= (cryptoLen + _session->_crypto->getMacInLen())))
            {
                Botan::secure_vector<Botan::byte> ourMac, hMac;
                _session->_crypto->computeMac(ourMac, decrypted, _rxSeq);
                hMac = Botan::secure_vector<Botan::byte>(_in.begin() + cryptoLen, _in.begin() + cryptoLen + _session->_crypto->getMacInLen());
                if (hMac != ourMac)
                {
                    _session->_logger->pushMessage(std::stringstream() << "Mismatched HMACs.");
                    return;// -1;
                }
                cryptoLen += _session->_crypto->getMacInLen();
            }
        }
        else
        {
            decrypted = _in;
        }
        if (decrypted.empty() == false)
        {
            _rxSeq++;
            std::unique_lock<std::mutex> lock(_inBufferMutex);
            _inBuffer.push(decrypted);
            _inBufferCondVar.notify_all();
            if (_in.size() == cryptoLen)
            {
                _in.clear();
            }
            else
            {
                _in.erase(_in.begin(), _in.begin() + cryptoLen);
            }
        }
    }

    std::cout << "thread exit" << std::endl;
}

short CppsshTransport::waitForPacket(Botan::byte command, CppsshPacket *packet)
{
    Botan::byte cmd;
    std::unique_lock<std::mutex> lock(_inBufferMutex);
    std::chrono::steady_clock::time_point t0 = std::chrono::steady_clock::now();
    while ((_running == true) && (std::chrono::steady_clock::now() < (t0 + std::chrono::seconds(_timeout))))
    {
        if (_inBuffer.size() > 0)
        {
            break;
        }
        _inBufferCondVar.wait_for(lock, std::chrono::microseconds(1));
    }
    packet->clear();
    if (_inBuffer.empty() == false)
    {
        packet->copy(_inBuffer.front());
        _inBuffer.pop();
    }
    if (packet->size() != 0)
    {
        cmd = packet->getCommand();
        if ((command == cmd) || (command == 0))
        {
            return cmd;
        }
        else
        {
            return 0;
        }
    }
    return command;
}
