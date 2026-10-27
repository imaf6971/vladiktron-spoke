// USB audio routing. Over USB the Model:Cycles is a 48 kHz stereo audio interface: it sends its main mix to the
// computer and plays what the computer sends it (mixed into its output). Here those two streams go to and come
// from audio devices on this Mac, chosen in the USB Audio menu: for example a loopback device (BlackHole and the
// like) so a DAW can record the Model:Cycles, or play into it. Each stream runs only while it is routed.
import AVFoundation
import CoreAudio

struct AudioDevice: Hashable, Identifiable {
    let id: AudioDeviceID
    let uid: String
    let name: String
}

enum AudioDevices {
    /// Devices with input (or output) channels, by name.
    static func list(input: Bool) -> [AudioDevice] {
        var addr = AudioObjectPropertyAddress(mSelector: kAudioHardwarePropertyDevices,
                                              mScope: kAudioObjectPropertyScopeGlobal, mElement: kAudioObjectPropertyElementMain)
        var size: UInt32 = 0
        guard AudioObjectGetPropertyDataSize(AudioObjectID(kAudioObjectSystemObject), &addr, 0, nil, &size) == noErr else { return [] }
        var ids = [AudioDeviceID](repeating: 0, count: Int(size) / MemoryLayout<AudioDeviceID>.size)
        guard AudioObjectGetPropertyData(AudioObjectID(kAudioObjectSystemObject), &addr, 0, nil, &size, &ids) == noErr else { return [] }
        return ids.compactMap { id -> AudioDevice? in
            guard channels(id, input: input) > 0, let uid = string(id, kAudioDevicePropertyDeviceUID),
                  let name = string(id, kAudioObjectPropertyName) else { return nil }
            return AudioDevice(id: id, uid: uid, name: name)
        }.sorted { $0.name.localizedCaseInsensitiveCompare($1.name) == .orderedAscending }
    }

    private static func channels(_ id: AudioDeviceID, input: Bool) -> Int {
        var addr = AudioObjectPropertyAddress(mSelector: kAudioDevicePropertyStreamConfiguration,
                                              mScope: input ? kAudioDevicePropertyScopeInput : kAudioDevicePropertyScopeOutput,
                                              mElement: kAudioObjectPropertyElementMain)
        var size: UInt32 = 0
        guard AudioObjectGetPropertyDataSize(id, &addr, 0, nil, &size) == noErr, size > 0 else { return 0 }
        let raw = UnsafeMutableRawPointer.allocate(byteCount: Int(size), alignment: 16)
        defer { raw.deallocate() }
        guard AudioObjectGetPropertyData(id, &addr, 0, nil, &size, raw) == noErr else { return 0 }
        let list = UnsafeMutableAudioBufferListPointer(raw.assumingMemoryBound(to: AudioBufferList.self))
        return list.reduce(0) { $0 + Int($1.mNumberChannels) }
    }

    private static func string(_ id: AudioDeviceID, _ selector: AudioObjectPropertySelector) -> String? {
        var addr = AudioObjectPropertyAddress(mSelector: selector, mScope: kAudioObjectPropertyScopeGlobal,
                                              mElement: kAudioObjectPropertyElementMain)
        var value: Unmanaged<CFString>?
        var size = UInt32(MemoryLayout<Unmanaged<CFString>?>.size)
        guard AudioObjectGetPropertyData(id, &addr, 0, nil, &size, &value) == noErr, let v = value else { return nil }
        return v.takeRetainedValue() as String
    }
}

final class UsbAudio: ObservableObject {
    /// Where the Model:Cycles' USB audio goes (an output device's UID), nil: nowhere.
    @Published var outputUID: String? { didSet { if outputUID != oldValue { save(); apply() } } }
    /// Which input device feeds the Model:Cycles' USB audio input (UID), nil: nothing.
    @Published var inputUID: String? { didSet { if inputUID != oldValue { save(); apply() } } }
    @Published private(set) var outputs: [AudioDevice] = []
    @Published private(set) var inputs: [AudioDevice] = []

    private var board: OpaquePointer?
    private var out: AudioOut?
    private var inEngine: AVAudioEngine?
    private var listener: AudioObjectPropertyListenerBlock?

    func start(board b: OpaquePointer) {
        board = b
        outputUID = UserDefaults.standard.string(forKey: "usbAudioOut")
        inputUID = UserDefaults.standard.string(forKey: "usbAudioIn")
        refresh()
        var addr = AudioObjectPropertyAddress(mSelector: kAudioHardwarePropertyDevices,
                                              mScope: kAudioObjectPropertyScopeGlobal, mElement: kAudioObjectPropertyElementMain)
        let block: AudioObjectPropertyListenerBlock = { [weak self] _, _ in self?.refresh() }
        AudioObjectAddPropertyListenerBlock(AudioObjectID(kAudioObjectSystemObject), &addr, .main, block)
        listener = block
        apply()
    }

    func stop() {
        if let block = listener {
            var addr = AudioObjectPropertyAddress(mSelector: kAudioHardwarePropertyDevices,
                                                  mScope: kAudioObjectPropertyScopeGlobal, mElement: kAudioObjectPropertyElementMain)
            AudioObjectRemovePropertyListenerBlock(AudioObjectID(kAudioObjectSystemObject), &addr, .main, block)
        }
        listener = nil
        stopStreams()
        if let b = board { mcb_usb_audio_enable(b, 0, 0) }
        board = nil
    }

    /// Device lists changed (or first look): update them, and restart the routes if a device came or went.
    private func refresh() {
        let o = AudioDevices.list(input: false), i = AudioDevices.list(input: true)
        let changed = o != outputs || i != inputs
        outputs = o
        inputs = i
        if changed && board != nil { apply() }
    }

    private func save() {
        UserDefaults.standard.set(outputUID, forKey: "usbAudioOut")
        UserDefaults.standard.set(inputUID, forKey: "usbAudioIn")
    }

    private func stopStreams() {
        out?.stop()
        out = nil
        inEngine?.inputNode.removeTap(onBus: 0)
        inEngine?.stop()
        inEngine = nil
    }

    private func apply() {
        guard let b = board else { return }
        stopStreams()
        let outDev = outputs.first { $0.uid == outputUID }
        let inDev = inputs.first { $0.uid == inputUID }
        mcb_usb_audio_enable(b, outDev != nil ? 1 : 0, inDev != nil ? 1 : 0)
        if let d = outDev {
            let o = AudioOut(name: "USB audio", read: { mcb_usb_audio_read(b, $0, $1) },
                             available: { mcb_usb_audio_read_available(b) })
            if o.start(device: d.id) { out = o }
        }
        if let d = inDev {
            AVCaptureDevice.requestAccess(for: .audio) { [weak self] granted in
                DispatchQueue.main.async {
                    guard granted, let self, self.board == b, self.inputUID == d.uid else { return }
                    self.startInput(device: d, board: b)
                }
            }
        }
    }

    /// Input device -> 48 kHz stereo -> the board's USB playback queue (about 10-50 ms ahead of it).
    private func startInput(device: AudioDevice, board b: OpaquePointer) {
        let engine = AVAudioEngine()
        do { try engine.inputNode.auAudioUnit.setDeviceID(device.id) } catch {
            NSLog("Spoke: USB audio input %@ failed: %@", device.name, "\(error)")
            return
        }
        let src = engine.inputNode.outputFormat(forBus: 0)
        guard src.channelCount > 0, src.sampleRate > 0,
              let dst = AVAudioFormat(commonFormat: .pcmFormatFloat32, sampleRate: AudioOut.rate, channels: 2, interleaved: true),
              let conv = AVAudioConverter(from: src, to: dst) else { return }
        if src.channelCount == 1 { conv.channelMap = [0, 0] }    // mono input on both sides
        engine.inputNode.installTap(onBus: 0, bufferSize: 512, format: src) { buffer, _ in
            let cap = AVAudioFrameCount(Double(buffer.frameLength) * AudioOut.rate / src.sampleRate) + 32
            guard let outBuf = AVAudioPCMBuffer(pcmFormat: dst, frameCapacity: cap) else { return }
            var fed = false
            conv.convert(to: outBuf, error: nil) { _, status in
                if fed { status.pointee = .noDataNow; return nil }
                fed = true
                status.pointee = .haveData
                return buffer
            }
            let n = Int(outBuf.frameLength)
            guard n > 0, let p = outBuf.floatChannelData?[0] else { return }
            if mcb_usb_audio_write_queued(b) > 2400 { return }   // the input runs fast: drop rather than lag
            _ = mcb_usb_audio_write(b, p, n)
        }
        do { try engine.start(); inEngine = engine } catch {
            engine.inputNode.removeTap(onBus: 0)
            NSLog("Spoke: USB audio input %@ failed: %@", device.name, "\(error)")
        }
    }
}
