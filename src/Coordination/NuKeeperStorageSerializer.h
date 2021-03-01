#pragma once
#include <libnuraft/nuraft.hxx> // Y_IGNORE
#include <Coordination/NuKeeperStorage.h>
#include <IO/WriteBuffer.h>
#include <IO/ReadBuffer.h>

namespace DB
{

enum SnapshotVersion : uint8_t
{
    V0 = 0,
};

struct NuKeeperStorageSnapshot
{
public:
    NuKeeperStorageSnapshot(NuKeeperStorage * storage_, size_t up_to_log_idx_);
    ~NuKeeperStorageSnapshot();

    static void serialize(const NuKeeperStorageSnapshot & snapshot, WriteBuffer & out);

    static void deserialize(NuKeeperStorage & storage, ReadBuffer & in);

    NuKeeperStorage * storage;

    SnapshotVersion version = SnapshotVersion::V0;
    size_t up_to_log_idx;
    int64_t zxid;
    int64_t session_id;
    size_t snapshot_container_size;
    NuKeeperStorage::Container::const_iterator begin;
    NuKeeperStorage::Container::const_iterator end;
    SessionAndTimeout session_and_timeout;
};

class NuKeeperSnapshotManager
{
public:
    explicit NuKeeperSnapshotManager(const std::string & snapshots_path_);

    size_t restoreFromLatestSnapshot(NuKeeperStorage * storage) const;

    std::string serializeSnapshotBufferToDisk(nuraft::buffer & buffer, size_t up_to_log_idx);
    nuraft::ptr<nuraft::buffer> deserializeSnapshotBufferFromDisk(size_t up_to_log_idx) const;

    static nuraft::ptr<nuraft::buffer> serializeSnapshotToBuffer(const NuKeeperStorageSnapshot & snapshot);
    static void deserializeSnapshotFromBuffer(NuKeeperStorage * storage, nuraft::ptr<nuraft::buffer> buffer);

private:
    const std::string snapshots_path;
    std::map<size_t, std::string> existing_snapshots;
};

}
