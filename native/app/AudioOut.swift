// Plays one of the board's 48 kHz stereo streams (the main output, or what it sends over USB) through Core Audio,
// on the default output device or a chosen one. The board paces its audio by the host clock, so its FIFO fills
// at the rate the device drains it; we keep a small cushion in front of the device and resync when the two
// drift apart. When the output hardware changes (headphones in, another default output, a new sample rate) the
// system stops the engine; it then starts again on the device as it is now. The engine is only ever stopped and
// started on `queue`: both wait for the hardware, for seconds while a device switches, and on the main thread that
// froze the window.
import AVFoundation
import CoreAudio

final class AudioOut {
    let engine = AVAudioEngine()                        // (internal: the tests post its configuration change)
    private var source: AVAudioSourceNode?
    private var tmp: UnsafeMutablePointer<Float>?       // the render block's scratch buffer, freed by stop()
    private var device: AudioDeviceID?                  // nil: the system's default output, followed when it changes
    private var observer: NSObjectProtocol?             // AVAudioEngineConfigurationChange
    private var defaultListener: AudioObjectPropertyListenerBlock?
    private var bound: AudioDeviceID?                   // the device the engine plays on now
    private(set) var restartedOnMain: Bool?             // (for the tests: where the last restart ran)
    private var steps: [String] = []                    // how long each part of the last (re)start took, for the log
    /// When the engine was last (re)started and when it next renders, for the log. (Written by the render thread;
    /// only read for a log line.)
    private final class FirstRender { var started = 0.0, rendered = 0.0 }
    private let firstRender = FirstRender()
    private let queue = DispatchQueue(label: "Vladiktron Spoke audio output", qos: .userInitiated)
    private let onQueueKey = DispatchSpecificKey<Bool>()
    private let name: String
    /// Told (on the output's own queue) when the output would not start, in a sentence for the user, and nil once it
    /// plays. `problemTitle` begins the sentence.
    var onProblem: ((String?) -> Void)?
    var problemTitle = "No sound"
    private var lastError: Error?
    private let read: (UnsafeMutablePointer<Float>, Int) -> Int
    private let available: () -> Int

    static let rate = 48000.0
    private static let format = AVAudioFormat(standardFormatWithSampleRate: rate, channels: 2)!
    private static var defaultOutputAddress = AudioObjectPropertyAddress(
        mSelector: kAudioHardwarePropertyDefaultOutputDevice, mScope: kAudioObjectPropertyScopeGlobal,
        mElement: kAudioObjectPropertyElementMain)
    static let cushion = 1024          // frames (~21 ms) buffered before playback (re)starts
    static let ceiling = 4096          // more than this queued: drop the excess so latency stays low

    /// `read(buffer, frames)` fills interleaved L,R and returns frames read; `available()` says how many wait.
    init(name: String, read: @escaping (UnsafeMutablePointer<Float>, Int) -> Int, available: @escaping () -> Int) {
        self.name = name
        self.read = read
        self.available = available
        queue.setSpecific(key: onQueueKey, value: true)
    }

    /// Runs `work` on `queue` and waits for it (at once if already there: deinit can run on it).
    private func onQueue<T>(_ work: () -> T) -> T {
        DispatchQueue.getSpecific(key: onQueueKey) == true ? work() : queue.sync(execute: work)
    }

    /// Starts on `device` (nil: the system's default output). Returns false if the device would not start.
    @discardableResult
    func start(device: AudioDeviceID? = nil) -> Bool { onQueue { startHere(device: device) } }

    private func startHere(device: AudioDeviceID?) -> Bool {
        let read = self.read, available = self.available, name = self.name
        let tmp = UnsafeMutablePointer<Float>.allocate(capacity: 2 * 8192)
        self.tmp = tmp
        var primed = false
        var played = 0, underruns = 0, drops = 0, lastLog = 0
        var peak: Float = 0
        let log = ProcessInfo.processInfo.environment["SPOKE_AUDIOLOG"] != nil
        let first = firstRender
        let node = AVAudioSourceNode(format: AudioOut.format) { _, _, frameCount, abl -> OSStatus in
            let out = UnsafeMutableAudioBufferListPointer(abl)
            let l = out[0].mData!.assumingMemoryBound(to: Float.self)
            let r = out[1].mData!.assumingMemoryBound(to: Float.self)
            let n = min(Int(frameCount), 8192)
            if first.rendered == 0 { first.rendered = ProcessInfo.processInfo.systemUptime }
            var avail = available()
            if avail > AudioOut.ceiling {
                var drop = avail - AudioOut.cushion
                while drop > 0 { let k = read(tmp, min(drop, 8192)); if k == 0 { break }; drop -= k }
                avail = AudioOut.cushion
                drops += 1
            }
            if !primed { primed = avail >= AudioOut.cushion }
            let got = primed ? read(tmp, n) : 0
            if got < n { if primed { underruns += 1 }; primed = false }  // underrun: wait for a new cushion
            played += got
            if log && played - lastLog >= 5 * 48000 {                       // SPOKE_AUDIOLOG: health every 5 s
                lastLog = played
                NSLog("Spoke %@: %.1f s played, %ld underruns, %ld drops, %ld queued, peak %.2f",
                      name, Double(played) / AudioOut.rate, underruns, drops, available(), peak)
                peak = 0
            }
            for i in 0..<got { l[i] = tmp[2 * i]; r[i] = tmp[2 * i + 1]; peak = max(peak, abs(l[i]), abs(r[i])) }
            for i in got..<Int(frameCount) { l[i] = 0; r[i] = 0 }
            return noErr
        }
        source = node
        self.device = device
        engine.attach(node)
        let notifications = OperationQueue()
        notifications.underlyingQueue = queue
        observer = NotificationCenter.default.addObserver(forName: .AVAudioEngineConfigurationChange, object: engine,
                                                          queue: notifications) { [weak self] _ in
            guard let self, !self.engine.isRunning else { return }   // (already started again)
            self.restart(because: "the output hardware changed")
        }
        if device == nil {
            let block: AudioObjectPropertyListenerBlock = { [weak self] _, _ in
                guard let self, AudioOut.defaultOutput() != self.bound else { return }
                self.restart(because: "the default output changed")
            }
            AudioObjectAddPropertyListenerBlock(AudioObjectID(kAudioObjectSystemObject), &AudioOut.defaultOutputAddress,
                                                queue, block)
            defaultListener = block
        }
        let ok = run()
        report(ok)
        return ok
    }

    private func report(_ ok: Bool) {
        guard !ok else { onProblem?(nil); return }
        let id = device ?? AudioOut.defaultOutput()
        let what = id.map(AudioOut.name) ?? "the default output"
        let code = (lastError as NSError?).map { " (Core Audio error \($0.code))" } ?? ""
        onProblem?("\(problemTitle): \(what) would not start\(code). It tries again when the output changes.")
    }

    /// Connects the stream to the output as it is now (its sample rate may have changed) and starts it.
    private func run() -> Bool {
        guard let node = source else { return false }
        do {
            let id = device ?? AudioOut.defaultOutput()
            try step("device") { if let id { try engine.outputNode.auAudioUnit.setDeviceID(id) } }
            bound = id
            step("connect") {
                engine.disconnectNodeOutput(node)
                engine.connect(node, to: engine.mainMixerNode, format: AudioOut.format)
            }
            firstRender.rendered = 0
            firstRender.started = ProcessInfo.processInfo.systemUptime
            try step("start") { try engine.start() }
            lastError = nil
            return true
        } catch {
            NSLog("Spoke: %@ audio output failed: %@", name, "\(error)")
            lastError = error
            return false
        }
    }

    /// The system stopped the engine, or the default output moved: start again on the output as it is now. A device
    /// that is still settling may refuse for a moment, so this tries for about two seconds.
    private func restart(because reason: String, tries: Int = 5) {
        guard source != nil else { return }             // stopped for good
        restartedOnMain = Thread.isMainThread
        let from = bound.map(AudioOut.describe) ?? "none"
        steps = []
        step("stop") { engine.stop() }
        let ok = run()
        let to = bound.map(AudioOut.describe) ?? "none"
        NSLog("Spoke %@: %@ (%@ -> %@); output %@: %@", name, reason, from, to, ok ? "restarted" : "did not start",
              steps.joined(separator: ", "))
        if ok {                                         // and when the device first asked for audio
            report(true)
            queue.asyncAfter(deadline: .now() + 3) { [first = firstRender, name] in
                let ms = first.rendered > 0 ? String(format: "%.0f ms", (first.rendered - first.started) * 1000) : "not yet"
                NSLog("Spoke %@: first audio after the restart's start: %@", name, ms)
            }
            return
        }
        guard tries > 1 else { NSLog("Spoke %@: the output would not start again", name); report(false); return }
        queue.asyncAfter(deadline: .now() + 0.5) { [weak self] in
            self?.restart(because: reason, tries: tries - 1)
        }
    }

    /// Runs `work` and notes in `steps` how long it took.
    private func step<T>(_ what: String, _ work: () throws -> T) rethrows -> T {
        let t0 = ProcessInfo.processInfo.systemUptime
        defer { steps.append(String(format: "%@ %.0f ms", what, (ProcessInfo.processInfo.systemUptime - t0) * 1000)) }
        return try work()
    }

    /// The device's name ("MacBook Pro Speakers"; "device 42" if it has none or is gone).
    private static func name(_ id: AudioDeviceID) -> String {
        var addr = AudioObjectPropertyAddress(mSelector: kAudioObjectPropertyName, mScope: kAudioObjectPropertyScopeGlobal,
                                              mElement: kAudioObjectPropertyElementMain)
        var name: Unmanaged<CFString>?
        var size = UInt32(MemoryLayout<Unmanaged<CFString>?>.size)
        return AudioObjectGetPropertyData(id, &addr, 0, nil, &size, &name) == noErr
            ? (name?.takeRetainedValue() as String?) ?? "device \(id)" : "device \(id)"
    }

    /// "Name @ 48000 Hz", for the log.
    private static func describe(_ id: AudioDeviceID) -> String {
        var addr = AudioObjectPropertyAddress(mSelector: kAudioDevicePropertyNominalSampleRate,
                                              mScope: kAudioObjectPropertyScopeGlobal, mElement: kAudioObjectPropertyElementMain)
        var rate = Float64(0), size = UInt32(MemoryLayout<Float64>.size)
        _ = AudioObjectGetPropertyData(id, &addr, 0, nil, &size, &rate)
        return String(format: "%@ @ %.0f Hz", name(id), rate)
    }

    private static func defaultOutput() -> AudioDeviceID? {
        var id = AudioDeviceID(0), size = UInt32(MemoryLayout<AudioDeviceID>.size)
        guard AudioObjectGetPropertyData(AudioObjectID(kAudioObjectSystemObject), &defaultOutputAddress, 0, nil,
                                         &size, &id) == noErr, id != 0 else { return nil }
        return id
    }

    func stop() { onQueue { stopHere() } }

    private func stopHere() {
        if let observer { NotificationCenter.default.removeObserver(observer) }
        observer = nil
        if let block = defaultListener {
            AudioObjectRemovePropertyListenerBlock(AudioObjectID(kAudioObjectSystemObject), &AudioOut.defaultOutputAddress,
                                                   queue, block)
        }
        defaultListener = nil
        engine.stop()
        if let s = source { engine.detach(s) }
        source = nil
        tmp?.deallocate()                               // after engine.stop: no render block runs any more
        tmp = nil
    }

    deinit { stop() }                                   // (UsbAudio drops an output that would not start)
}
