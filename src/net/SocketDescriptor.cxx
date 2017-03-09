/*
 * Copyright (C) 2012-2015 Max Kellermann <max.kellermann@gmail.com>
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * - Redistributions of source code must retain the above copyright
 * notice, this list of conditions and the following disclaimer.
 *
 * - Redistributions in binary form must reproduce the above copyright
 * notice, this list of conditions and the following disclaimer in the
 * documentation and/or other materials provided with the
 * distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * ``AS IS'' AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
 * FOR A PARTICULAR PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL THE
 * FOUNDATION OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
 * (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
 * SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT,
 * STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED
 * OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "SocketDescriptor.hxx"
#include "SocketAddress.hxx"
#include "StaticSocketAddress.hxx"

#ifdef HAVE_POSIX
#include <sys/socket.h>
#else
#include <winsock2.h>
#include <ws2tcpip.h>
#endif

#ifndef HAVE_POSIX

void
SocketDescriptor::Close()
{
	if (IsDefined())
		::closesocket(Steal());
}

#endif

SocketDescriptor
SocketDescriptor::Accept()
{
#if defined(__linux__) && !defined(__BIONIC__) && !defined(KOBO)
	int connection_fd = ::accept4(Get(), nullptr, nullptr, SOCK_CLOEXEC);
#else
	int connection_fd = ::accept(Get(), nullptr, nullptr);
#endif
	return connection_fd >= 0
		? SocketDescriptor(connection_fd)
		: Undefined();
}

bool
SocketDescriptor::Connect(SocketAddress address)
{
	assert(address.IsDefined());

	return ::connect(Get(), address.GetAddress(), address.GetSize()) >= 0;
}

bool
SocketDescriptor::Create(int domain, int type, int protocol)
{
#ifdef WIN32
	static bool initialised = false;
	if (!initialised) {
		WSADATA data;
		WSAStartup(MAKEWORD(2,2), &data);
		initialised = true;
	}
#endif

#ifdef SOCK_CLOEXEC
	/* implemented since Linux 2.6.27 */
	type |= SOCK_CLOEXEC;
#endif

	int new_fd = socket(domain, type, protocol);
	if (new_fd < 0)
		return false;

	Set(new_fd);
	return true;
}

bool
SocketDescriptor::Bind(SocketAddress address)
{
	return bind(Get(), address.GetAddress(), address.GetSize()) == 0;
}

#ifdef __linux__

bool
SocketDescriptor::AutoBind()
{
	static constexpr sa_family_t family = AF_LOCAL;
	return Bind(SocketAddress((const struct sockaddr *)&family,
				  sizeof(family)));
}

#endif

ssize_t
SocketDescriptor::Read(void *buffer, size_t length)
{
	int flags = 0;
#ifdef HAVE_POSIX
	flags |= MSG_DONTWAIT;
#endif

	return ::recv(Get(), (char *)buffer, length, flags);
}

ssize_t
SocketDescriptor::Write(const void *buffer, size_t length)
{
	int flags = 0;
#ifdef __linux__
	flags |= MSG_NOSIGNAL;
#endif

	return ::send(Get(), (const char *)buffer, length, flags);
}

#ifndef HAVE_POSIX

int
SocketDescriptor::WaitReadable(int timeout_ms) const
{
	assert(IsDefined());

	fd_set rfds;
	FD_ZERO(&rfds);
	FD_SET(Get(), &rfds);

	struct timeval timeout, *timeout_p = nullptr;
	if (timeout_ms >= 0) {
		timeout.tv_sec = unsigned(timeout_ms) / 1000;
		timeout.tv_usec = (unsigned(timeout_ms) % 1000) * 1000;
		timeout_p = &timeout;
	}

	return select(Get() + 1, &rfds, nullptr, nullptr, timeout_p);
}

int
SocketDescriptor::WaitWritable(int timeout_ms) const
{
	assert(IsDefined());

	fd_set wfds;
	FD_ZERO(&wfds);
	FD_SET(Get(), &wfds);

	struct timeval timeout, *timeout_p = nullptr;
	if (timeout_ms >= 0) {
		timeout.tv_sec = unsigned(timeout_ms) / 1000;
		timeout.tv_usec = (unsigned(timeout_ms) % 1000) * 1000;
		timeout_p = &timeout;
	}

	return select(Get() + 1, nullptr, &wfds, nullptr, timeout_p);
}

#endif

ssize_t
SocketDescriptor::Read(void *buffer, size_t length,
		       StaticSocketAddress &address)
{
	int flags = 0;
#ifdef HAVE_POSIX
	flags |= MSG_DONTWAIT;
#endif

	socklen_t addrlen = address.GetCapacity();
	ssize_t nbytes = ::recvfrom(Get(), (char *)buffer, length, flags,
				    address, &addrlen);
	if (nbytes > 0)
		address.SetSize(addrlen);

	return nbytes;
}

ssize_t
SocketDescriptor::Write(const void *buffer, size_t length,
			SocketAddress address)
{
	int flags = 0;
#ifdef HAVE_POSIX
	flags |= MSG_DONTWAIT;
#endif
#ifdef __linux__
	flags |= MSG_NOSIGNAL;
#endif

	return ::sendto(Get(), (const char *)buffer, length, flags,
			address.GetAddress(), address.GetSize());
}
