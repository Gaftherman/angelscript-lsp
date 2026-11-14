// @expect: accept
namespace Citadel {
    class GateLock {
        private int KeyPin;
        void unlockGate() {
            int p = this.KeyPin;
        }
    }
}
