namespace dasync {

// Map of pid_t to void *, with possibility of reserving entries so that mappings can
// be later added with no danger of allocator exhaustion (bad_alloc).
class pid_map
{
    using pair = std::pair<pid_t, void *>;
    std::unordered_map<pid_t, void *> base_map;
    std::vector<pair> backup_vector;
    
    // Number of entries in backup_vector that are actually in use (as opposed
    // to simply reserved):
    int backup_size = 0;
    
    public:
    using entry = std::pair<bool, void *>;

    entry get(pid_t key) noexcept
    {
        auto it = base_map.find(key);
        if (it == base_map.end()) {
            // Not in map; look in vector
            for (int i = 0; i < backup_size; i++) {
                if (backup_vector[i].first == key) {
                    return entry(true, backup_vector[i].second);
                }
            }
        
            return entry(false, nullptr);
        }
        
        return entry(true, it->second);
    }
    
    entry erase(pid_t key) noexcept
    {
        auto iter = base_map.find(key);
        if (iter != base_map.end()) {
            entry r(true, iter->second);
            base_map.erase(iter);
            return r;
        }
        for (int i = 0; i < backup_size; i++) {
            if (backup_vector[i].first == key) {
                entry r(true, backup_vector[i].second);
                backup_vector.erase(backup_vector.begin() + i);
                return r;
            }
        }
        return entry(false, nullptr);
    }
    
    // Throws bad_alloc on reservation failure
    void reserve()
    {
        backup_vector.resize(backup_vector.size() + 1);
    }
    
    void add(pid_t key, void *val) // throws std::bad_alloc
    {
        base_map[key] = val;
    }
    
    void add_from_reserve(pid_t key, void *val) noexcept
    {
        try {
            base_map[key] = val;
            backup_vector.resize(backup_vector.size() - 1);
        }
        catch (std::bad_alloc &) {
            // We couldn't add into the map, use the reserve:
            backup_vector[backup_size++] = pair(key, val);
        }
    }
};

template <class Base> class ChildProcEvents : public Base
{
    private:
    pid_map child_waiters;

    using SigInfo = typename Base::SigInfo;
    
    protected:
    void receiveSignal(SigInfo &siginfo, void *userdata)
    {
        if (siginfo.get_signo() == SIGCHLD) {
            int status;
            pid_t child;
            while ((child = waitpid(-1, &status, WNOHANG)) > 0) {
                pid_map::entry ent = child_waiters.erase(child);
                if (ent.first) {
                    Base::receiveChildStat(child, status, ent.second);
                }
            }
        }
        else {
            Base::receiveSignal(siginfo, userdata);
        }
    }
    
    public:
    void reserveChildWatch()
    {
        child_waiters.reserve();
    }
    
    void addChildWatch(pid_t child, void *val)
    {
        child_waiters.add(child, val);
    }
    
    void addReservedChildWatch(pid_t child, void *val) noexcept
    {
        child_waiters.add_from_reserve(child, val);
    }
    
    template <typename T> void init(T *loop_mech)
    {
        loop_mech->addSignalWatch(SIGCHLD, nullptr);
    }
};


} // end namespace
