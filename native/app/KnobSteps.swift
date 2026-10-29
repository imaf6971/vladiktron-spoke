// Value steps on the Model:Cycles' encoders: one OS knob event per step (a full quadrature cycle is two events, and
// while a knob moves the OS counts nearly every event: a scroll click moved two values). The OS weighs slower events
// less (measured, 8 clicks: 6-8 values 50-200 ms apart, 5-6 at 300 ms, 3-4 at 600 ms) and, once a knob has been
// still for about 1.2 s, ignores its first event (it shows the parameter). So after such a pause a wake-up event goes
// first and the steps 40 ms later (two events closer together can count twice); steps that come in meanwhile go
// with them. LEVEL/DATA (encoder 1) counts its first event too: it gets no wake-up.
import Foundation

final class KnobSteps: @unchecked Sendable {
    private let nudge: (Int, Int) -> Void            // encoder, OS knob events
    private let lock = NSLock()
    private var lastStep: [Int: TimeInterval] = [:]  // encoder -> time of its last step
    private var waking: [Int: TimeInterval] = [:]    // encoder -> end of its wake-up wait
    private var deferred: [Int: Int] = [:]           // steps held for the end of that wait

    /// Still this long, the OS has forgotten the knob (a lone event 0.3 s after a turn usually counts, after 1.2 s never).
    static let pause: TimeInterval = 1.2

    init(nudge: @escaping (Int, Int) -> Void) { self.nudge = nudge }

    /// `n` value steps of encoder `enc` (any thread).
    func step(_ enc: Int, _ n: Int) {
        guard n != 0 else { return }
        if enc == 1 { nudge(enc, n); return }
        lock.lock(); defer { lock.unlock() }
        let now = ProcessInfo.processInfo.systemUptime
        defer { lastStep[enc] = now }
        if let until = waking[enc], now < until { deferred[enc, default: 0] += n; return }
        if now - (lastStep[enc] ?? 0) < KnobSteps.pause { nudge(enc, n); return }
        nudge(enc, n.signum())
        waking[enc] = now + 0.04
        deferred[enc] = n
        DispatchQueue.global(qos: .userInteractive).asyncAfter(deadline: .now() + 0.04) { [weak self] in
            guard let self else { return }
            self.lock.lock(); defer { self.lock.unlock() }
            if let n = self.deferred.removeValue(forKey: enc), n != 0 { self.nudge(enc, n) }
            self.waking[enc] = nil
        }
    }
}
