// Included inside xgameruntime_offline.cpp's anonymous namespace, after APP_LOCAL_DEVICE_ID. Public contract:
// learn.microsoft.com/gaming/gdk/docs/reference/system/xuser/functions/xuserdeviceassociationchangedcallback
// and structs/xuserdeviceassociationchange. Only this runtime's explicitly added offline user can be paired.

enum class DeviceEvent : uint32_t { Association = 0, AudioEndpoint = 1 };

struct DeviceAssociationChange {
    APP_LOCAL_DEVICE_ID deviceId;
    uint64_t oldUser;
    uint64_t newUser;
};
static_assert(sizeof(DeviceAssociationChange) == 48);
using DeviceAssociationCallback = void(CALLBACK*)(void*, const DeviceAssociationChange*);

struct DeviceRegistration {
    uint64_t token = 0;
    DeviceEvent kind = DeviceEvent::Association;
    XTaskQueueHandle queue = nullptr;
    void* context = nullptr;
    void* callback = nullptr;
    std::mutex mutex;
    std::condition_variable idle;
    std::list<DeviceAssociationChange> pending;
    bool active = true;
    bool scheduled = false;
    unsigned running = 0;

    ~DeviceRegistration() { if (queue) XTaskQueueCloseHandle(queue); }
};

// GameInput's poller is process-lifetime. Keep its observed association/registration storage alive too: ordinary
// CRT static destruction must not free a vector while that poller can still notify. Explicit unregistration still
// closes queues and releases registrations normally; the OS reclaims remaining process handles on exit.
struct DeviceAssociationState {
    SRWLOCK lock = SRWLOCK_INIT;
    std::vector<std::shared_ptr<DeviceRegistration>> registrations;
    rt::input_user::DeviceSnapshot associated;
    uint64_t next_token = 1;
};
DeviceAssociationState& g_device_state = *new DeviceAssociationState;
SRWLOCK& g_device_lock = g_device_state.lock;
auto& g_device_registrations = g_device_state.registrations;
auto& g_associated_devices = g_device_state.associated;
uint64_t& g_device_next_token = g_device_state.next_token;
thread_local DeviceRegistration* g_current_association_callback = nullptr;

void remove_inactive_device_registration(const std::shared_ptr<DeviceRegistration>& registration) {
    AcquireSRWLockExclusive(&g_device_lock);
    {
        std::lock_guard lock(registration->mutex);
        if (!registration->active && registration->running == 0) {
            const auto found = std::find(g_device_registrations.begin(), g_device_registrations.end(), registration);
            if (found != g_device_registrations.end()) g_device_registrations.erase(found);
        }
    }
    ReleaseSRWLockExclusive(&g_device_lock);
}

struct AssociationDispatch { std::shared_ptr<DeviceRegistration> registration; };

struct RunningAssociationCallback {
    const std::shared_ptr<DeviceRegistration>& registration;
    DeviceRegistration* previous = g_current_association_callback;
    explicit RunningAssociationCallback(const std::shared_ptr<DeviceRegistration>& value) : registration(value) {
        g_current_association_callback = registration.get();
    }
    ~RunningAssociationCallback() {
        g_current_association_callback = previous;
        std::lock_guard lock(registration->mutex);
        --registration->running;
        registration->idle.notify_all();
    }
};

void CALLBACK deliver_device_associations(void* context, bool cancelled) {
    const std::unique_ptr<AssociationDispatch> dispatch(static_cast<AssociationDispatch*>(context));
    const auto& registration = dispatch->registration;
    for (;;) {
        DeviceAssociationChange change{};
        {
            std::lock_guard lock(registration->mutex);
            if (cancelled || !registration->active || registration->pending.empty()) {
                registration->scheduled = false;
                registration->pending.clear();
                break;
            }
            change = registration->pending.front();
            registration->pending.pop_front();
            ++registration->running;
        }
        const RunningAssociationCallback running(registration);
        reinterpret_cast<DeviceAssociationCallback>(registration->callback)(registration->context, &change);
    }
    remove_inactive_device_registration(registration);
}

bool schedule_device_associations(std::unique_ptr<AssociationDispatch> dispatch) {
    const auto registration = dispatch->registration;
    XTaskQueueHandle queue = nullptr;
    {
        std::lock_guard lock(registration->mutex);
        if (!registration->active || registration->scheduled || registration->pending.empty()) return true;
        const HRESULT duplicated = XTaskQueueDuplicateHandle(registration->queue, &queue);
        if (FAILED(duplicated)) {
            static std::atomic<unsigned> failures{0};
            if (failures.fetch_add(1, std::memory_order_relaxed) < 16)
                std::fprintf(stderr, "[rt] input_user: cannot retain pairing queue hr=0x%08lx\n",
                             static_cast<unsigned long>(duplicated));
            return false;
        }
        registration->scheduled = true;
    }
    // The queue may use Immediate dispatch. No registry/input/registration lock is held, and the local queue
    // duplicate survives a callback unregistering itself. A queued job owns only the registration and copied IDs.
    AssociationDispatch* submitted = dispatch.release();
    const HRESULT result = XTaskQueueSubmitCallback(queue, XTaskQueuePort::Completion, submitted,
                                                    deliver_device_associations);
    XTaskQueueCloseHandle(queue);
    if (FAILED(result)) {
        delete submitted;
        {
            std::lock_guard lock(registration->mutex);
            registration->scheduled = false;
        }
        static std::atomic<unsigned> failures{0};
        if (failures.fetch_add(1, std::memory_order_relaxed) < 16)
            std::fprintf(stderr, "[rt] input_user: pairing callback submission failed hr=0x%08lx\n",
                         static_cast<unsigned long>(result));
        return false;
    }
    return true;
}

bool refresh_device_associations() {
    std::vector<std::unique_ptr<AssociationDispatch>> dispatches;
    AcquireSRWLockExclusive(&g_device_lock);
    try {
        // Snapshot under the same serialization as comparison/commit: concurrent add and poll notifications must
        // not publish an older device snapshot after a newer association. The provider only takes input's mutex;
        // its callers invoke this listener after releasing both input and poll locks.
        auto connected = rt::input_user::user_added.load(std::memory_order_acquire)
            ? rt::input_user::connected_devices() : rt::input_user::DeviceSnapshot{};
        std::vector<DeviceAssociationChange> changes;
        struct PendingBatch {
            std::shared_ptr<DeviceRegistration> registration;
            std::list<DeviceAssociationChange> changes;
            std::unique_ptr<AssociationDispatch> dispatch;
        };
        std::vector<PendingBatch> batches;
        batches.reserve(g_device_registrations.size());
        dispatches.reserve(g_device_registrations.size());
        const auto contains = [](const auto& ids, const auto& id) {
            return std::find(ids.begin(), ids.end(), id) != ids.end();
        };
        const auto change_for = [](const rt::input_user::DeviceId& id, uint64_t old_user, uint64_t new_user) {
            DeviceAssociationChange change{};
            std::memcpy(change.deviceId.value, id.data(), id.size());
            change.oldUser = old_user;
            change.newUser = new_user;
            return change;
        };
        for (const auto& id : g_associated_devices)
            if (!contains(connected, id)) changes.push_back(change_for(id, 1, 0));
        for (const auto& id : connected)
            if (!contains(g_associated_devices, id)) changes.push_back(change_for(id, 0, 1));
        for (const auto& registration : g_device_registrations) {
            if (registration->kind != DeviceEvent::Association) continue;
            std::lock_guard lock(registration->mutex);
            if (!registration->active || (changes.empty() && (registration->pending.empty() || registration->scheduled)))
                continue;
            batches.push_back({registration, {}, std::make_unique<AssociationDispatch>(AssociationDispatch{registration})});
            for (const auto& change : changes) batches.back().changes.push_back(change);
        }
        // Allocate every event before changing the observed association set. Splicing prepared nodes is noexcept,
        // so an allocation failure cannot leave some registrations holding a partially committed event sequence.
        for (auto& batch : batches) {
            std::lock_guard lock(batch.registration->mutex);
            if (!batch.registration->active) continue;
            batch.registration->pending.splice(batch.registration->pending.end(), batch.changes);
            dispatches.push_back(std::move(batch.dispatch));
        }
        g_associated_devices = std::move(connected);
    } catch (const std::bad_alloc&) {
        ReleaseSRWLockExclusive(&g_device_lock);
        static std::atomic<unsigned> failures{0};
        if (failures.fetch_add(1, std::memory_order_relaxed) < 16)
            std::fprintf(stderr, "[rt] input_user: pairing snapshot/event allocation failed\n");
        return false;
    } catch (...) {
        ReleaseSRWLockExclusive(&g_device_lock);
        throw;
    }
    ReleaseSRWLockExclusive(&g_device_lock);
    bool complete = true;
    for (auto& dispatch : dispatches)
        if (!schedule_device_associations(std::move(dispatch))) complete = false;
    return complete;
}

struct InstallDeviceAssociationListener {
    InstallDeviceAssociationListener() {
        rt::input_user::change_listener.store(refresh_device_associations, std::memory_order_release);
    }
} g_install_device_association_listener;

template <DeviceEvent Kind>
HRESULT STDMETHODCALLTYPE device_register(void*, XTaskQueueHandle queue, void* context, void* callback,
                                          XTaskQueueRegistrationToken* token) {
    if (!callback || !token) return E_INVALIDARG;
    token->token = 0;
    std::shared_ptr<DeviceRegistration> registration;
    try { registration = std::make_shared<DeviceRegistration>(); }
    catch (...) { return E_OUTOFMEMORY; }
    if (queue) {
        const HRESULT result = XTaskQueueDuplicateHandle(queue, &registration->queue);
        if (FAILED(result)) return result;
    } else if (!XTaskQueueGetCurrentProcessTaskQueue(&registration->queue)) {
        return E_NO_TASK_QUEUE;
    }
    registration->kind = Kind;
    registration->context = context;
    registration->callback = callback;
    AcquireSRWLockExclusive(&g_device_lock);
    try {
        registration->token = g_device_next_token;
        g_device_registrations.push_back(registration);
        token->token = g_device_next_token++;
    } catch (...) {
        ReleaseSRWLockExclusive(&g_device_lock);
        return E_OUTOFMEMORY;
    }
    ReleaseSRWLockExclusive(&g_device_lock);
    return S_OK;
}

template <DeviceEvent Kind>
bool STDMETHODCALLTYPE device_unregister(void*, XTaskQueueRegistrationToken token, bool wait) {
    std::shared_ptr<DeviceRegistration> registration;
    AcquireSRWLockShared(&g_device_lock);
    for (const auto& entry : g_device_registrations)
        if (entry->token == token.token && entry->kind == Kind) { registration = entry; break; }
    ReleaseSRWLockShared(&g_device_lock);
    if (!registration) return true;
    XTaskQueueHandle queue = nullptr;
    bool complete = false;
    {
        std::unique_lock lock(registration->mutex);
        registration->active = false;
        registration->pending.clear();
        queue = registration->queue;
        registration->queue = nullptr;
        // Queued jobs become harmless without waiting for a manually dispatched queue: they check active before
        // touching title state. Only an executing callback must finish. Waiting inside that same callback would
        // deadlock, so disable it now and report the still-running unregistration as pending.
        if (wait && g_current_association_callback != registration.get())
            registration->idle.wait(lock, [&] { return registration->running == 0; });
        complete = registration->running == 0;
    }
    if (queue) XTaskQueueCloseHandle(queue);
    if (complete) remove_inactive_device_registration(registration);
    return complete;
}
