// Model Mac: Elektron's Model:Cycles OS 1.13 on the virtual board, with a front panel like the hardware.
// Firmware and storage live in ~/Library/Application Support/ModelMac (installed by native/build.sh from
// the user's own .syx); nothing from the firmware is part of this app.
import AppKit
import SwiftUI

final class AppDelegate: NSObject, NSApplicationDelegate {
    let board = Board()

    func applicationDidFinishLaunching(_ notification: Notification) {
        board.snapshotView = { NSApp.windows.first?.contentView }
        board.start()
        DispatchQueue.main.async {
            guard let w = NSApp.windows.first else { return }
            w.contentAspectRatio = Panel.size                        // the panel never letterboxes
            w.backgroundColor = NSColor(red: 0.600, green: 0.604, blue: 0.588, alpha: 1)
            w.setContentSize(NSSize(width: 1100, height: 1100 * Panel.size.height / Panel.size.width))
        }
        if ProcessInfo.processInfo.environment["MODELMAC_DEMO"] != nil { demo() }
    }

    /// Self-check: once the OS has drawn its screen for a few seconds, drives the panel like a user would
    /// (pad T3, PLAY, a DECAY turn), then saves a snapshot while the parameter popup is up.
    private func demo() {
        let b = board
        DispatchQueue.global().async {
            while b.screen == nil { usleep(100_000) }
            sleep(4)                                                  // let the OS finish its boot work
            let steps: [(Double, () -> Void)] = [
                (0.0, { b.pad(3, pressure: 0.9) }), (0.3, { b.pad(3, pressure: 0) }),
                (1.0, { b.key(Panel.play, down: true) }), (1.3, { b.key(Panel.play, down: false) }),
                (2.5, { b.turn(3, steps: 6) }), (2.62, { b.snapshotNow() }),
            ]
            for (t, f) in steps { DispatchQueue.main.asyncAfter(deadline: .now() + t, execute: f) }
        }
    }

    func applicationWillTerminate(_ notification: Notification) { board.stop() }
    func applicationShouldTerminateAfterLastWindowClosed(_ sender: NSApplication) -> Bool { true }
}

@main
struct ModelMacApp: App {
    @NSApplicationDelegateAdaptor(AppDelegate.self) var delegate

    var body: some Scene {
        WindowGroup("Model:Cycles") {
            PanelView(board: delegate.board)
                .ignoresSafeArea()
                .frame(minWidth: 750, minHeight: 501)
        }
        .windowStyle(.hiddenTitleBar)
        .windowResizability(.contentMinSize)
        .commands { CommandMenu("USB Audio") { UsbAudioMenu(usb: delegate.board.usbAudio) } }
    }
}

/// The Model:Cycles as a USB audio interface: where its USB output goes, and what feeds its USB input.
struct UsbAudioMenu: View {
    @ObservedObject var usb: UsbAudio

    var body: some View {
        Picker("Model:Cycles to", selection: $usb.outputUID) {
            Text("Nowhere").tag(String?.none)
            ForEach(usb.outputs) { Text($0.name).tag(Optional($0.uid)) }
        }
        Picker("Into Model:Cycles from", selection: $usb.inputUID) {
            Text("Nothing").tag(String?.none)
            ForEach(usb.inputs) { Text($0.name).tag(Optional($0.uid)) }
        }
    }
}
