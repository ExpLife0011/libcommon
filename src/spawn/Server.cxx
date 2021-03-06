/*
 * Copyright 2007-2017 Content Management AG
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

#include "Server.hxx"
#include "Config.hxx"
#include "IProtocol.hxx"
#include "Parser.hxx"
#include "Builder.hxx"
#include "Prepared.hxx"
#include "Hook.hxx"
#include "MountList.hxx"
#include "CgroupState.hxx"
#include "Direct.hxx"
#include "Registry.hxx"
#include "ExitListener.hxx"
#include "event/SocketEvent.hxx"
#include "net/UniqueSocketDescriptor.hxx"
#include "net/ReceiveMessage.hxx"
#include "io/UniqueFileDescriptor.hxx"
#include "util/DeleteDisposer.hxx"
#include "util/ConstBuffer.hxx"
#include "util/StaticArray.hxx"
#include "util/PrintException.hxx"
#include "util/Exception.hxx"

#include <boost/intrusive/list.hpp>

#include <system_error>
#include <algorithm>
#include <memory>
#include <map>

#include <unistd.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <signal.h>
#include <poll.h>
#include <string.h>
#include <errno.h>

class SpawnServerProcess;

class SpawnFdList {
	std::forward_list<UniqueFileDescriptor> list;

public:
	SpawnFdList(std::forward_list<UniqueFileDescriptor> &&_list) noexcept
		:list(std::move(_list)) {}

	SpawnFdList(SpawnFdList &&src) = default;

	SpawnFdList &operator=(SpawnFdList &&src) = default;

	bool IsEmpty() {
		return list.empty();
	}

	size_t size() const {
		return std::distance(list.begin(), list.end());
	}

	UniqueFileDescriptor Get() {
		if (IsEmpty())
			throw MalformedSpawnPayloadError();

		auto result = std::move(list.front());
		list.pop_front();
		return result;
	}

	UniqueSocketDescriptor GetSocket() {
		return UniqueSocketDescriptor(Get().Steal());
	}
};

class SpawnServerConnection;

class SpawnServerChild final : public ExitListener {
	SpawnServerConnection &connection;

	const int id;

	const pid_t pid;

	const std::string name;

public:
	explicit SpawnServerChild(SpawnServerConnection &_connection,
				  int _id, pid_t _pid,
				  const char *_name)
		:connection(_connection), id(_id), pid(_pid), name(_name) {}

	SpawnServerChild(const SpawnServerChild &) = delete;
	SpawnServerChild &operator=(const SpawnServerChild &) = delete;

	const char *GetName() const {
		return name.c_str();
	}

	void Kill(ChildProcessRegistry &child_process_registry, int signo) {
		child_process_registry.Kill(pid, signo);
	}

	/* virtual methods from ExitListener */
	void OnChildProcessExit(int status) override;

	/* boost::instrusive::set hooks */
	typedef boost::intrusive::set_member_hook<boost::intrusive::link_mode<boost::intrusive::normal_link>> IdHook;
	IdHook id_hook;

	struct CompareId {
		bool operator()(const SpawnServerChild &a, const SpawnServerChild &b) const {
			return a.id < b.id;
		}

		bool operator()(int a, const SpawnServerChild &b) const {
			return a < b.id;
		}

		bool operator()(const SpawnServerChild &a, int b) const {
			return a.id < b;
		}
	};
};

class SpawnServerConnection
	: public boost::intrusive::list_base_hook<boost::intrusive::link_mode<boost::intrusive::normal_link>> {
	SpawnServerProcess &process;
	UniqueSocketDescriptor socket;

	const LLogger logger;

	SocketEvent event;

	typedef boost::intrusive::set<SpawnServerChild,
				      boost::intrusive::member_hook<SpawnServerChild,
								    SpawnServerChild::IdHook,
								    &SpawnServerChild::id_hook>,
				      boost::intrusive::compare<SpawnServerChild::CompareId>> ChildIdMap;
	ChildIdMap children;

public:
	SpawnServerConnection(SpawnServerProcess &_process,
			      UniqueSocketDescriptor &&_socket);
	~SpawnServerConnection();

	void OnChildProcessExit(int id, int status, SpawnServerChild *child);

private:
	void RemoveConnection();

	void SendExit(int id, int status);
	void SpawnChild(int id, const char *name, PreparedChildProcess &&p);

	void HandleExecMessage(SpawnPayload payload, SpawnFdList &&fds);
	void HandleKillMessage(SpawnPayload payload, SpawnFdList &&fds);
	void HandleMessage(ConstBuffer<uint8_t> payload, SpawnFdList &&fds);
	void HandleMessage(ReceiveMessageResult &&result);

	void ReadEventCallback(unsigned events);
};

void
SpawnServerChild::OnChildProcessExit(int status)
{
	connection.OnChildProcessExit(id, status, this);
}

void
SpawnServerConnection::OnChildProcessExit(int id, int status,
					  SpawnServerChild *child)
{
	children.erase(children.iterator_to(*child));
	delete child;

	SendExit(id, status);
}

class SpawnServerProcess {
	const SpawnConfig config;
	const CgroupState &cgroup_state;
	SpawnHook *const hook;

	const LLogger logger;

	EventLoop loop;

	ChildProcessRegistry child_process_registry;

	typedef boost::intrusive::list<SpawnServerConnection,
				       boost::intrusive::constant_time_size<false>> ConnectionList;
	ConnectionList connections;

public:
	SpawnServerProcess(const SpawnConfig &_config,
			   const CgroupState &_cgroup_state,
			   SpawnHook *_hook)
		:config(_config), cgroup_state(_cgroup_state), hook(_hook),
		 logger("spawn"),
		 child_process_registry(loop) {}

	const SpawnConfig &GetConfig() const {
		return config;
	}

	const CgroupState &GetCgroupState() const {
		return cgroup_state;
	}

	EventLoop &GetEventLoop() {
		return loop;
	}

	ChildProcessRegistry &GetChildProcessRegistry() {
		return child_process_registry;
	}

	bool Verify(const PreparedChildProcess &p) const {
		return hook != nullptr && hook->Verify(p);
	}

	void AddConnection(UniqueSocketDescriptor &&_socket) {
		auto connection = new SpawnServerConnection(*this, std::move(_socket));
		connections.push_back(*connection);
	}

	void RemoveConnection(SpawnServerConnection &connection) {
		connections.erase_and_dispose(connections.iterator_to(connection),
					      DeleteDisposer());

		if (connections.empty())
			/* all connections are gone */
			Quit();
	}

	void Run();

private:
	void Quit() {
		assert(connections.empty());

		child_process_registry.SetVolatile();
	}
};

SpawnServerConnection::SpawnServerConnection(SpawnServerProcess &_process,
					     UniqueSocketDescriptor &&_socket)
	:process(_process), socket(std::move(_socket)),
	 logger("spawn"),
	 event(process.GetEventLoop(), socket.Get(),
	       SocketEvent::READ|SocketEvent::PERSIST,
	       BIND_THIS_METHOD(ReadEventCallback)) {
	event.Add();
}

SpawnServerConnection::~SpawnServerConnection()
{
	event.Delete();

	auto &registry = process.GetChildProcessRegistry();
	children.clear_and_dispose([&registry](SpawnServerChild *child){
			child->Kill(registry, SIGTERM);
			delete child;
		});
}

inline void
SpawnServerConnection::RemoveConnection()
{
	process.RemoveConnection(*this);
}

void
SpawnServerConnection::SendExit(int id, int status)
{
	SpawnSerializer s(SpawnResponseCommand::EXIT);
	s.WriteInt(id);
	s.WriteInt(status);

	try {
		try {
			::Send<1>(socket, s);
		} catch (const std::system_error &e) {
			if (e.code().category() == std::system_category() &&
			    e.code().value() == EAGAIN) {
				/* the client may be busy, while the datagram queue
				   has filled (see /proc/sys/net/unix/max_dgram_qlen);
				   wait some more before giving up */
				struct pollfd pfd;
				pfd.fd = socket.Get();
				pfd.events = POLLOUT;

				static const struct timespec timeout = {10, 0};

				/* ignore all signals while waiting, or else the poll
				   may be interrupted too early by the next SIGCHLD */
				sigset_t signals;
				sigfillset(&signals);

				if (ppoll(&pfd, 1, &timeout, &signals) > 0) {
					/* try again (may throw another exception) */
					::Send<1>(socket, s);
					/* yay, it worked! */
					return;
				}
			}

			throw;
		}
	} catch (...) {
		logger(1, "Failed to send EXIT to worker: ",
		       GetFullMessage(std::current_exception()).c_str());
		RemoveConnection();
	}
}

inline void
SpawnServerConnection::SpawnChild(int id, const char *name,
				  PreparedChildProcess &&p)
{
	const auto &config = process.GetConfig();

	if (!p.uid_gid.IsEmpty()) {
		try {
			if (!process.Verify(p))
				config.Verify(p.uid_gid);
		} catch (const std::exception &e) {
			PrintException(e);
			SendExit(id, W_EXITCODE(0xff, 0));
			return;
		}
	}

	if (p.uid_gid.IsEmpty()) {
		if (config.default_uid_gid.IsEmpty()) {
			logger(1, "No uid/gid specified");
			SendExit(id, W_EXITCODE(0xff, 0));
			return;
		}

		p.uid_gid = config.default_uid_gid;
	}

	pid_t pid;

	try {
		pid = SpawnChildProcess(std::move(p),
					process.GetCgroupState());
	} catch (...) {
		logger(1, "Failed to spawn child process: ",
		       GetFullMessage(std::current_exception()).c_str());
		SendExit(id, W_EXITCODE(0xff, 0));
		return;
	}

	auto *child = new SpawnServerChild(*this, id, pid, name);
	children.insert(*child);

	process.GetChildProcessRegistry().Add(pid, name, child);
}

static void
Read(SpawnPayload &payload, ResourceLimits &rlimits)
{
	unsigned i = payload.ReadByte();
	struct rlimit &data = rlimits.values[i];
	payload.ReadT(data);
}

static void
Read(SpawnPayload &payload, UidGid &uid_gid)
{
	payload.ReadT(uid_gid.uid);
	payload.ReadT(uid_gid.gid);

	const size_t n_groups = payload.ReadByte();
	if (n_groups > uid_gid.groups.max_size())
		throw MalformedSpawnPayloadError();

	for (size_t i = 0; i < n_groups; ++i)
		payload.ReadT(uid_gid.groups[i]);

	if (n_groups < uid_gid.groups.max_size())
		uid_gid.groups[n_groups] = 0;
}

inline void
SpawnServerConnection::HandleExecMessage(SpawnPayload payload,
					 SpawnFdList &&fds)
{
	int id;
	payload.ReadInt(id);
	const char *name = payload.ReadString();

	PreparedChildProcess p;

	MountList **mount_tail = &p.ns.mount.mounts;
	assert(*mount_tail == nullptr);

	std::forward_list<MountList> mounts;
	std::forward_list<std::string> strings;
	std::forward_list<CgroupOptions::SetItem> cgroup_sets;

	while (!payload.IsEmpty()) {
		const SpawnExecCommand cmd = (SpawnExecCommand)payload.ReadByte();
		switch (cmd) {
		case SpawnExecCommand::ARG:
			if (p.args.size() >= 16384)
				throw MalformedSpawnPayloadError();

			p.Append(payload.ReadString());
			break;

		case SpawnExecCommand::SETENV:
			if (p.env.size() >= 16384)
				throw MalformedSpawnPayloadError();

			p.PutEnv(payload.ReadString());
			break;

		case SpawnExecCommand::UMASK:
			{
				uint16_t value;
				payload.ReadT(value);
				p.umask = value;
			}

			break;

		case SpawnExecCommand::STDIN:
			p.SetStdin(fds.Get().Steal());
			break;

		case SpawnExecCommand::STDOUT:
			p.SetStdout(fds.Get().Steal());
			break;

		case SpawnExecCommand::STDERR:
			p.SetStderr(fds.Get().Steal());
			break;

		case SpawnExecCommand::STDERR_PATH:
			p.stderr_path = payload.ReadString();
			break;

		case SpawnExecCommand::CONTROL:
			p.SetControl(fds.Get().Steal());
			break;

		case SpawnExecCommand::TTY:
			p.tty = true;
			break;

		case SpawnExecCommand::REFENCE:
			p.refence.Set(payload.ReadString());
			break;

		case SpawnExecCommand::USER_NS:
			p.ns.enable_user = true;
			break;

		case SpawnExecCommand::PID_NS:
			p.ns.enable_pid = true;
			break;

		case SpawnExecCommand::NETWORK_NS:
			p.ns.enable_network = true;
			break;

		case SpawnExecCommand::NETWORK_NS_NAME:
			p.ns.network_namespace = payload.ReadString();
			break;

		case SpawnExecCommand::IPC_NS:
			p.ns.enable_ipc = true;
			break;

		case SpawnExecCommand::MOUNT_NS:
			p.ns.mount.enable_mount = true;
			break;

		case SpawnExecCommand::MOUNT_PROC:
			p.ns.mount.mount_proc = true;
			break;

		case SpawnExecCommand::WRITABLE_PROC:
			p.ns.mount.writable_proc = true;
			break;

		case SpawnExecCommand::PIVOT_ROOT:
			p.ns.mount.pivot_root = payload.ReadString();
			break;

		case SpawnExecCommand::MOUNT_HOME:
			p.ns.mount.mount_home = payload.ReadString();
			p.ns.mount.home = payload.ReadString();
			break;

		case SpawnExecCommand::MOUNT_TMP_TMPFS:
			p.ns.mount.mount_tmp_tmpfs = payload.ReadString();
			break;

		case SpawnExecCommand::MOUNT_TMPFS:
			p.ns.mount.mount_tmpfs = payload.ReadString();
			break;

		case SpawnExecCommand::BIND_MOUNT:
			{
				const char *source = payload.ReadString();
				const char *target = payload.ReadString();
				bool writable = payload.ReadByte();
				bool exec = payload.ReadByte();
				mounts.emplace_front(source, target, false,
						     writable, exec);
			}

			*mount_tail = &mounts.front();
			mount_tail = &mounts.front().next;
			break;

		case SpawnExecCommand::HOSTNAME:
			p.ns.hostname = payload.ReadString();
			break;

		case SpawnExecCommand::RLIMIT:
			Read(payload, p.rlimits);
			break;

		case SpawnExecCommand::UID_GID:
			Read(payload, p.uid_gid);
			break;

		case SpawnExecCommand::SCHED_IDLE_:
			p.sched_idle = true;
			break;

		case SpawnExecCommand::IOPRIO_IDLE:
			p.ioprio_idle = true;
			break;

		case SpawnExecCommand::FORBID_USER_NS:
			p.forbid_user_ns = true;
			break;

		case SpawnExecCommand::FORBID_MULTICAST:
			p.forbid_multicast = true;
			break;

		case SpawnExecCommand::FORBID_BIND:
			p.forbid_bind = true;
			break;

		case SpawnExecCommand::NO_NEW_PRIVS:
			p.no_new_privs = true;
			break;

		case SpawnExecCommand::CGROUP:
			p.cgroup.name = payload.ReadString();
			break;

		case SpawnExecCommand::CGROUP_SET:
			{
				const char *set_name = payload.ReadString();
				const char *set_value = payload.ReadString();
				strings.emplace_front(set_name);
				set_name = strings.front().c_str();
				strings.emplace_front(set_value);
				set_value = strings.front().c_str();

				cgroup_sets.emplace_front(set_name, set_value);
				auto &set = cgroup_sets.front();
				set.next = p.cgroup.set_head;
				p.cgroup.set_head = &set;
			}

			break;

		case SpawnExecCommand::PRIORITY:
			payload.ReadInt(p.priority);
			break;

		case SpawnExecCommand::CHROOT:
			p.chroot = payload.ReadString();
			break;

		case SpawnExecCommand::CHDIR:
			p.chdir = payload.ReadString();
			break;

		case SpawnExecCommand::HOOK_INFO:
			p.hook_info = payload.ReadString();
			break;
		}
	}

	SpawnChild(id, name, std::move(p));
}

inline void
SpawnServerConnection::HandleKillMessage(SpawnPayload payload,
					 SpawnFdList &&fds)
{
	if (!fds.IsEmpty())
		throw MalformedSpawnPayloadError();

	int id, signo;
	payload.ReadInt(id);
	payload.ReadInt(signo);
	if (!payload.IsEmpty())
		throw MalformedSpawnPayloadError();

	auto i = children.find(id, SpawnServerChild::CompareId());
	if (i == children.end())
		return;

	SpawnServerChild *child = &*i;
	children.erase(i);

	child->Kill(process.GetChildProcessRegistry(), signo);
	delete child;
}

inline void
SpawnServerConnection::HandleMessage(ConstBuffer<uint8_t> payload,
				     SpawnFdList &&fds)
{
	const auto cmd = (SpawnRequestCommand)payload.shift();

	switch (cmd) {
	case SpawnRequestCommand::CONNECT:
		if (!payload.empty() || fds.size() != 1)
			throw MalformedSpawnPayloadError();

		process.AddConnection(fds.GetSocket());
		break;

	case SpawnRequestCommand::EXEC:
		HandleExecMessage(SpawnPayload(payload), std::move(fds));
		break;

	case SpawnRequestCommand::KILL:
		HandleKillMessage(SpawnPayload(payload), std::move(fds));
		break;
	}
}

inline void
SpawnServerConnection::HandleMessage(ReceiveMessageResult &&result)
{
	HandleMessage(ConstBuffer<uint8_t>::FromVoid(result.payload),
		      std::move(result.fds));
}

inline void
SpawnServerConnection::ReadEventCallback(unsigned)
try {
	ReceiveMessageBuffer<8192, CMSG_SPACE(sizeof(int) * 32)> rmb;

	auto result = ReceiveMessage(socket, rmb, MSG_DONTWAIT);
	if (result.payload.empty()) {
		RemoveConnection();
		return;
	}

	try {
		HandleMessage(std::move(result));
	} catch (MalformedSpawnPayloadError) {
		logger(3, "Malformed spawn payload");
	}
} catch (...) {
	logger(2, std::current_exception());
	RemoveConnection();
}

inline void
SpawnServerProcess::Run()
{
	loop.Dispatch();
}

void
RunSpawnServer(const SpawnConfig &config, const CgroupState &cgroup_state,
	       SpawnHook *hook,
	       UniqueSocketDescriptor socket)
{
	if (cgroup_state.IsEnabled()) {
		/* tell the client that the cgroups feature is available;
		   there is no other way for the client to know if we don't
		   tell him; see SpawnServerClient::SupportsCgroups() */
		static constexpr auto cmd = SpawnResponseCommand::CGROUPS_AVAILABLE;
		send(socket.Get(), &cmd, sizeof(cmd), MSG_NOSIGNAL);
	}

	SpawnServerProcess process(config, cgroup_state, hook);
	process.AddConnection(std::move(socket));
	process.Run();
}
