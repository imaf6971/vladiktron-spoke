// Plays one of the board's 48 kHz stereo streams (the main output, or what it sends over USB) through Core Audio,
// on the default output device or a chosen one. The board paces its audio by the host clock, so its FIFO fills
// at the rate the device drains it; we keep a small cushion in front of the device and resync when the two
// drift apart. When the output hardware changes (headphones in, another default output, a new sample rate) the
// system stops the engine; it then starts again on the device as it is now.
import AVFoundation
import CoreAudio

final class AudioOut {
    let engine = AVAudioEngine()                        // (internal: the tests post its configuration change)
    private var source: AVAudioSourceNode?
    private var tmp: UnsafeMutablePointer<Float>?       // the render block's scratch buffer, freed by stop()
    private var device: AudioDeviceID?                  // nil: the system's default output, followed when it changes
    private var observer: NSObjectProtocol?             // AVAudioEngineConfigurationChange
    private var defaultListener: AudioObjectPropertyListenerBlock?
    private let name: String
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
    }

    /// Starts on `device` (nil: the system's default output). Returns false if the device would not start.
    @discardableResult
    func start(device: AudioDeviceID? = nil) -> Bool {
        let read = self.read, available = self.available, name = self.name
        let tmp = UnsafeMutablePointer<Float>.allocate(capacity: 2 * 8192)
        self.tmp = tmp
        var primed = false
        var played = 0, underruns = 0, drops = 0, lastLog = 0
        var peak: Float = 0
        let log = ProcessInfo.processInfo.environment["SPOKE_AUDIOLOG"] != nil
        let node = AVAudioSourceNode(format: AudioOut.format) { _, _, frameCount, abl -> OSStatus in
            let out = UnsafeMutableAudioBufferListPointer(abl)
            let l = out[0].mData!.assumingMemoryBound(to: Float.self)
            let r = out[1].mData!.assumingMemoryBound(to: Float.self)
            let n = min(Int(frameCount), 8192)
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
        observer = NotificationCenter.default.addObserver(forName: .AVAudioEngineConfigurationChange, object: engine,
                                                          queue: .main) { [weak self] _ in
            self?.restart(because: "the output hardware changed")
        }
        if device == nil {
            let block: AudioObjectPropertyListenerBlock = { [weak self] _, _ in
                self?.restart(because: "the default output changed")
            }
            AudioObjectAddPropertyListenerBlock(AudioObjectID(kAudioObjectSystemObject), &AudioOut.defaultOutputAddress,
                                                .main, block)
            defaultListener = block
        }
        return run()
    }

    /// Connects the stream to the output as it is now (its sample rate may have changed) and starts it.
    private func run() -> Bool {
        guard let node = source else { return false }
        do {
            if let id = device ?? AudioOut.defaultOutput() { try engine.outputNode.auAudioUnit.setDeviceID(id) }
            engine.disconnectNodeOutput(node)
            engine.connect(node, to: engine.mainMixerNode, format: AudioOut.format)
            try engine.start()
            return true
        } catch {
            NSLog("Spoke: %@ audio output failed: %@", name, "\(error)")
            return false
        }
    }

    /// The system stopped the engine, or the default output moved: start again on the output as it is now. A device
    /// that is still settling may refuse for a moment, so this tries for about two seconds.
    private func restart(because reason: String, tries: Int = 5) {
        guard source != nil else { return }             // stopped for good
        NSLog("Spoke %@: %@; restarting the output", name, reason)
        engine.stop()
        if run() { return }
        guard tries > 1 else { NSLog("Spoke %@: the output would not start again", name); return }
        DispatchQueue.main.asyncAfter(deadline: .now() + 0.5) { [weak self] in
            self?.restart(because: reason, tries: tries - 1)
        }
    }

    private static func defaultOutput() -> AudioDeviceID? {
        var id = AudioDeviceID(0), size = UInt32(MemoryLayout<AudioDeviceID>.size)
        guard AudioObjectGetPropertyData(AudioObjectID(kAudioObjectSystemObject), &defaultOutputAddress, 0, nil,
                                         &size, &id) == noErr, id != 0 else { return nil }
        return id
    }

    func stop() {
        if let observer { NotificationCenter.default.removeObserver(observer) }
        observer = nil
        if let block = defaultListener {
            AudioObjectRemovePropertyListenerBlock(AudioObjectID(kAudioObjectSystemObject), &AudioOut.defaultOutputAddress,
                                                   .main, block)
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
