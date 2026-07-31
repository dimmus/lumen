module;

import lx.foundation;
import lx.runtime;
import lx.drm;

export module lx.compositor:hotplug;

import :output;

export namespace lx::compositor {

class hotplug_monitor {
public:
    [[nodiscard]] lx::result<void> start(output_manager& outputs, lx::drm::kms_device& kms);
    void poll();

private:
    output_manager* outputs_ = nullptr;
    lx::drm::kms_device* kms_ = nullptr;
    bool running_ = false;
};

} // namespace lx::compositor


lx::result<void> lx::compositor::hotplug_monitor::start(output_manager& outputs,
                                                        lx::drm::kms_device& kms) {
    outputs_ = &outputs;
    kms_ = &kms;
    running_ = true;
    outputs_->refresh_from_drm(kms);
    return {};
}

void lx::compositor::hotplug_monitor::poll() {
    if (!running_ || !outputs_ || !kms_)
        return;
    outputs_->refresh_from_drm(*kms_);
}
