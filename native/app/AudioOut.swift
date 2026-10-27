// Plays one of the board's 48 kHz stereo streams (the main output, or what it sends over USB) through Core Audio,
// on the default output device or a chosen one. The board paces its audio by the host clock, so its FIFO fills
// at the rate the device drains it; we keep a small cushion in front of the device and resync when the two
// drift apart.
import AVFoundation
import CoreAudio

final class AudioOut {
    private let engine = AVAudioEngine()
    private var source: AVAudioSourceNode?
    private let name: String
    private let read: (UnsafeMutablePointer<Float>, Int) -> Int
    private let available: () -> Int

    static let rate = 48000.0
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
        let fmt = AVAudioFormat(standardFormatWithSampleRate: AudioOut.rate, channels: 2)!
        let tmp = UnsafeMutablePointer<Float>.allocate(capacity: 2 * 8192)
        var primed = false
        var played = 0, underruns = 0, drops = 0, lastLog = 0
        var peak: Float = 0
        let log = ProcessInfo.processInfo.environment["SPOKE_AUDIOLOG"] != nil
        let node = AVAudioSourceNode(format: fmt) { _, _, frameCount, abl -> OSStatus in
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
        do {
            if let device { try engine.outputNode.auAudioUnit.setDeviceID(device) }
            engine.attach(node)
            engine.connect(node, to: engine.mainMixerNode, format: fmt)
            try engine.start()
            return true
        } catch {
            NSLog("Spoke: %@ audio output failed: %@", name, "\(error)")
            return false
        }
    }

    func stop() {
        engine.stop()
        if let s = source { engine.detach(s) }
        source = nil
    }
}
