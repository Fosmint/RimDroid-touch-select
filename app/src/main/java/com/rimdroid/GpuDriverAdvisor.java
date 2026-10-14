package com.rimdroid;

/**
 * Detects the device GPU once and applies the matching recommended Vulkan driver to a freshly
 * created instance, so a new instance doesn't start on the System driver (which black-screens many
 * Adreno GPUs). The caller shows {@link Result} to the user and can offer "change in settings".
 */
public final class GpuDriverAdvisor {

    private GpuDriverAdvisor() {}

    public static final class Result {
        public final String gpuName;     // e.g. "Adreno (TM) 610"
        public final String driverLabel; // e.g. "Turnip Adreno 6xx (legacy)"
        public final String driverSo;    // soName written to the instance
        public final boolean applied;    // false if the instance already had an explicit driver
        /** Tell the player: only when the driver was set AND the instance will use it. The native
         *  engine always draws with the phone's own Vulkan, so the message there was noise; the
         *  driver is still written, ready if the instance is switched to box64 later. */
        public final boolean announce;
        Result(String gpuName, String driverLabel, String driverSo, boolean applied, boolean announce) {
            this.gpuName = gpuName;
            this.driverLabel = driverLabel;
            this.driverSo = driverSo;
            this.applied = applied;
            this.announce = announce;
        }
    }

    /**
     * Query the GPU and store the recommended driver on {@code instanceName} (unless it already has
     * an explicit driver). Runs a small EGL context — call OFF the UI thread.
     */
    public static Result applyRecommendedDriver(String instanceName) {
        GpuInfo gpu = GpuInfo.query();
        String so = gpu.recommendedDriverSo();
        int idx = 0;
        for (int i = 0; i < LauncherPreferences.VULKAN_DRIVERS.size(); i++) {
            if (LauncherPreferences.VULKAN_DRIVERS.get(i).soName.equals(so)) { idx = i; break; }
        }
        String label = LauncherPreferences.VULKAN_DRIVERS.get(idx).label;
        InstanceSettings s = new InstanceSettings(instanceName);
        boolean applied = false;
        if (!s.hasExplicitDriver()) {
            s.setVulkanDriverSo(so);
            applied = true;
        }
        return new Result(gpu.displayName(), label, so, applied, applied && !runsNative(instanceName));
    }

    /** The instance will start on the native engine (1.6, module present, engine chosen). */
    public static boolean runsNative(String instanceName) {
        try {
            com.rimdroid.game.GameInstanceManager mgr = com.rimdroid.game.GameInstanceManager.requireSingleton();
            mgr.reload();
            com.rimdroid.game.GameInstance gi = mgr.getByName(instanceName);
            return gi != null && com.rimdroid.game.NativeEngine.isSupported(gi)
                    && gi.settings().getEngine() == InstanceSettings.ENGINE_NATIVE;
        } catch (RuntimeException e) {
            return false;
        }
    }
}
