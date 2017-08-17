/*
 * author: Max Kellermann <mk@cm4all.com>
 */

#ifndef BENG_PROXY_NAMESPACE_OPTIONS_HXX
#define BENG_PROXY_NAMESPACE_OPTIONS_HXX

#include "translation/Features.hxx"

#include "util/Compiler.h"

class AllocatorPtr;
struct MountList;
struct SpawnConfig;
struct UidGid;
class MatchInfo;

struct NamespaceOptions {
    /**
     * Start the child process in a new user namespace?
     */
    bool enable_user = false;

    /**
     * Start the child process in a new PID namespace?
     */
    bool enable_pid = false;

    /**
     * Start the child process in a new Cgroup namespace?
     */
    bool enable_cgroup = false;

    /**
     * Start the child process in a new network namespace?
     */
    bool enable_network = false;

    /**
     * Start the child process in a new IPC namespace?
     */
    bool enable_ipc = false;

    bool enable_mount = false;

    /**
     * Mount a tmpfs to "/"?  All required mountpoints will be
     * created, but the filesystem will contain nothing else.
     */
    bool mount_root_tmpfs = false;

    /**
     * Mount a new /proc?
     */
    bool mount_proc = false;

    /**
     * Shall /proc we writable?  Only used if #mount_proc is set.
     */
    bool writable_proc = false;

    /**
     * Mount /dev/pts?
     */
    bool mount_pts = false;

    /**
     * Bind-mount the old /dev/pts?
     *
     * Note that #MountList cannot be used here because it enforces
     * MS_NODEV.
     */
    bool bind_mount_pts = false;

    /**
     * The name of the network namespace (/run/netns/X) to reassociate
     * with.  Requires #enable_network.
     */
    const char *network_namespace = nullptr;

    const char *pivot_root = nullptr;

    const char *home = nullptr;

#if TRANSLATION_ENABLE_EXPAND
    const char *expand_home = nullptr;
#endif

    /**
     * Mount the given home directory?  Value is the mount point.
     */
    const char *mount_home = nullptr;

    /**
     * Mount a new tmpfs on /tmp?  A non-empty string specifies
     * additional mount options, such as "size=64M".
     */
    const char *mount_tmp_tmpfs = nullptr;

    const char *mount_tmpfs = nullptr;

    MountList *mounts = nullptr;

    /**
     * The hostname of the new UTS namespace.
     */
    const char *hostname = nullptr;

    NamespaceOptions() = default;
    NamespaceOptions(AllocatorPtr alloc, const NamespaceOptions &src);

#if TRANSLATION_ENABLE_EXPAND
    gcc_pure
    bool IsExpandable() const;

    /**
     * Throws std::runtime_error on error.
     */
    void Expand(AllocatorPtr alloc, const MatchInfo &match_info);
#endif

    gcc_pure
    int GetCloneFlags(int flags) const;

    void SetupUidGidMap(const UidGid &uid_gid,
                        int pid) const;

    /**
     * Apply #network_namespace.
     */
    void ReassociateNetwork() const;

    /**
     * Throws std::system_error on error.
     */
    void Setup(const UidGid &uid_gid) const;

    char *MakeId(char *p) const;

    const char *GetJailedHome() const {
        return mount_home != nullptr
            ? mount_home
            : home;
    }

private:
    constexpr bool HasBindMount() const {
        return bind_mount_pts || mount_home != nullptr || mounts != nullptr;
    }
};

#endif
