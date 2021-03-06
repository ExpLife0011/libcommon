/*
 * Copyright 2017 Content Management AG
 * All rights reserved.
 *
 * author: Max Kellermann <mk@cm4all.com>
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

#include "Channel.hxx"
#include "Handler.hxx"
#include "Error.hxx"
#include "event/SocketEvent.hxx"
#include "net/AllocatedSocketAddress.hxx"
#include "util/Cancellable.hxx"

#include <assert.h>
#include <string.h>
#include <netinet/in.h>

namespace Cares {

class Channel::Socket {
		Channel &channel;
		SocketEvent event;

	public:
		Socket(Channel &_channel,
		       evutil_socket_t fd, unsigned events) noexcept
			:channel(_channel),
			 event(channel.GetEventLoop(), fd, events,
			       BIND_THIS_METHOD(OnSocket)) {
			event.Add(nullptr);
		}

		~Socket() noexcept {
			event.Delete();
		}

	private:
		void OnSocket(unsigned events) noexcept {
			channel.OnSocket(event.GetFd(), events);
		}
	};

Channel::Channel(EventLoop &event_loop)
	:defer_process(event_loop, BIND_THIS_METHOD(DeferredProcess)),
	 timeout_event(event_loop, BIND_THIS_METHOD(OnTimeout))
{
	int code = ares_init(&channel);
	if (code != 0)
		throw Error(code, "ares_init() failed");

	UpdateSockets();
}

Channel::~Channel() noexcept
{
	timeout_event.Cancel();
	defer_process.Cancel();
	ares_destroy(channel);
}

void
Channel::UpdateSockets() noexcept
{
	timeout_event.Cancel();
	sockets.clear();
	FD_ZERO(&read_ready);
	FD_ZERO(&write_ready);

	fd_set rfds, wfds;
	FD_ZERO(&rfds);
	FD_ZERO(&wfds);

	int max = ares_fds(channel, &rfds, &wfds);
	for (int i = 0; i < max; ++i) {
		unsigned events = 0;

		if (FD_ISSET(i, &rfds))
			events |= SocketEvent::READ;

		if (FD_ISSET(i, &wfds))
			events |= SocketEvent::WRITE;

		if (events != 0)
			sockets.emplace_front(*this, i, events);
	}

	struct timeval timeout_buffer;
	const auto *t = ares_timeout(channel, nullptr, &timeout_buffer);
	if (t != nullptr)
		timeout_event.Add(*t);
}

void
Channel::DeferredProcess() noexcept
{
	ares_process(channel, &read_ready, &write_ready);
	UpdateSockets();
}

void
Channel::OnSocket(evutil_socket_t fd, unsigned events) noexcept
{
	if (events & SocketEvent::READ)
		FD_SET(fd, &read_ready);

	if (events & SocketEvent::WRITE)
		FD_SET(fd, &write_ready);

	ScheduleProcess();
}

void
Channel::OnTimeout() noexcept
{
	ScheduleProcess();
}

template<typename F>
static void
AsSocketAddress(const struct hostent &he, F &&f)
{
	switch (he.h_addrtype) {
	case AF_INET:
		{
			struct sockaddr_in sin;
			memset(&sin, 0, sizeof(sin));
			memcpy(&sin.sin_addr, he.h_addr_list[0], he.h_length);
			sin.sin_family = AF_INET;

			f(SocketAddress((const struct sockaddr *)&sin,
					sizeof(sin)));
		}
		break;

	case AF_INET6:
		{
			struct sockaddr_in6 sin;
			memset(&sin, 0, sizeof(sin));
			memcpy(&sin.sin6_addr, he.h_addr_list[0], he.h_length);
			sin.sin6_family = AF_INET6;

			f(SocketAddress((const struct sockaddr *)&sin,
					sizeof(sin)));
		}
		break;

	default:
		throw std::runtime_error("Unsupported address type");
	}
}

class Channel::Request final : Cancellable {
	Handler *handler;

public:
	Request(Handler &_handler,
		CancellablePointer &cancel_ptr)
		:handler(&_handler) {
		cancel_ptr = *this;
	}

	void Start(ares_channel _channel,
		   const char *name, int family) noexcept {
		assert(handler != nullptr);

		ares_gethostbyname(_channel, name, family,
				   HostCallback, this);
	}

private:
	void Cancel() noexcept override {
		assert(handler != nullptr);

		handler = nullptr;
	}

	void HostCallback(int status, struct hostent *he) noexcept;

	static void HostCallback(void *arg, int status, int,
				 struct hostent *hostent) noexcept {
		auto &request = *(Request *)arg;
		request.HostCallback(status, hostent);
	}
};

inline void
Channel::Request::HostCallback(int status, struct hostent *he) noexcept
{
	assert(handler != nullptr);

	try {
		if (status != ARES_SUCCESS)
			throw Error(status, "ares_gethostbyname() failed");
		else if (he != nullptr)
			AsSocketAddress(*he, [&handler = *handler](SocketAddress address){
					handler.OnCaresSuccess(address);
				});
		else
			throw std::runtime_error("ares_gethostbyname() failed");
	} catch (...) {
		handler->OnCaresError(std::current_exception());
	}

	delete this;
}

void
Channel::Lookup(const char *name, Handler &handler,
		CancellablePointer &cancel_ptr) noexcept
{
	auto *request = new Request(handler, cancel_ptr);
	request->Start(channel, name, AF_UNSPEC);
	UpdateSockets();
}

} // namespace Cares
