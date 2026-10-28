// Vladiktron Spoke: Elektron's Model:Cycles OS 1.13 on the virtual board, with a front panel like the hardware.
// Firmware and storage live in ~/Library/Application Support/Vladiktron Spoke, installed from the user's own .syx on first
// run (FirmwareSetup.swift); nothing from the firmware is part of this app.
import AppKit
import SwiftUI

final class AppDelegate: NSObject, NSApplicationDelegate {
    let board = Board()

    func applicationDidFinishLaunching(_ notification: Notification) {
        Board.moveModelMacData()
        board.snapshotView = { NSApp.windows.first?.contentView }
        board.start()
        DispatchQueue.main.async {
            guard let w = NSApp.windows.first else { return }
            w.contentAspectRatio = Panel.size                        // the panel never letterboxes
            w.backgroundColor = NSColor(red: 0.600, green: 0.604, blue: 0.588, alpha: 1)
            w.setContentSize(NSSize(width: 1100, height: 1100 * Panel.size.height / Panel.size.width))
        }
        if ProcessInfo.processInfo.environment["SPOKE_DEMO"] != nil { demo() }
        // Self-check of the first-run install: SPOKE_INSTALL=/path/to.syx (or .zip) installs it after 3 s.
        if let file = ProcessInfo.processInfo.environment["SPOKE_INSTALL"] {
            DispatchQueue.main.asyncAfter(deadline: .now() + 3) { [board] in
                board.installFirmware(from: URL(fileURLWithPath: file)) { NSLog("Spoke test: install: %@", $0 ?? "ok") }
            }
        }
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

    /// Quitting powers the Model:Cycles off the way its power key does, so the OS saves its working state to the
    /// +Drive first (a couple of seconds, with "Shutting down..." on the screen).
    func applicationShouldTerminate(_ sender: NSApplication) -> NSApplication.TerminateReply {
        guard board.running else { return .terminateNow }
        board.quitting = true
        board.shutDown { NSApp.reply(toApplicationShouldTerminate: true) }
        return .terminateLater
    }
    func applicationWillTerminate(_ notification: Notification) { board.stop() }
    func applicationShouldTerminateAfterLastWindowClosed(_ sender: NSApplication) -> Bool { true }

    // MARK: About and licences

    static let source = URL(string: "https://github.com/imaf6971/vladiktron-spoke")!

    static func showAbout() {
        let body = NSFont.systemFont(ofSize: NSFont.smallSystemFontSize)
        let credits = NSMutableAttributedString()
        func add(_ text: String, link: URL? = nil, bold: Bool = false) {
            var attrs: [NSAttributedString.Key: Any] = [
                .font: bold ? NSFont.boldSystemFont(ofSize: body.pointSize) : body, .foregroundColor: NSColor.labelColor]
            if let link { attrs[.link] = link }
            credits.append(NSAttributedString(string: text, attributes: attrs))
        }
        add("Runs Elektron's own Model:Cycles OS 1.13, installed from your own OS file; no part of it comes with the app.\n\n")
        add("Free software under the GNU General Public License, version 2. Source, with the Unicorn patches: ")
        add("github.com/imaf6971/vladiktron-spoke", link: AppDelegate.source)
        add("\n\nBuilt with ")
        add("Unicorn", link: URL(string: "https://www.unicorn-engine.org/"))
        add(" (GPL-2.0) and code after ")
        add("Modded-Cycles", link: URL(string: "https://github.com/18nelli18/Modded-Cycles"))
        add(", elektron-model-tweaks and elektron-firmware-tool (MIT). Help → Licences has the full texts.\n\n")
        add("Model:Cycles and Elektron are trademarks of Elektron Music Machines MAV AB. Not affiliated with or endorsed by Elektron.")
        NSApp.orderFrontStandardAboutPanel(options: [.credits: credits])
        NSApp.activate(ignoringOtherApps: true)
    }

    /// Opens the licence texts that build.sh puts in the bundle's Resources.
    static func showLicences() {
        let files = ["LICENSE", "THIRD-PARTY-NOTICES.md"].compactMap { Bundle.main.url(forResource: $0, withExtension: nil) }
        guard !files.isEmpty else { NSWorkspace.shared.open(AppDelegate.source); return }
        let textEdit = URL(fileURLWithPath: "/System/Applications/TextEdit.app")
        NSWorkspace.shared.open(files, withApplicationAt: textEdit, configuration: NSWorkspace.OpenConfiguration())
    }

    // MARK: the Model:Cycles menu

    @objc func revealDataFolder() {
        NSWorkspace.shared.activateFileViewerSelecting([Board.dataDir])
    }

    /// Installs the OS again from the .syx (or Elektron's .zip), for a data folder that lost its files.
    @objc func installFirmware() {
        guard let url = FirmwareFile.choose() else { return }
        board.installFirmware(from: url) { [weak self] problem in
            if let problem {
                self?.report(NSError(domain: "Spoke", code: 2, userInfo: [NSLocalizedDescriptionKey: problem]))
            }
        }
    }

    @objc func resetDrive() {
        guard confirm("Reset the +Drive?",
                      detail: "The Model:Cycles shuts down and starts again with an empty +Drive, which the OS then " +
                              "formats with its factory content. All projects and samples on the +Drive are erased. " +
                              "Settings are kept.",
                      button: "Reset +Drive") else { return }
        board.resetDrive { [weak self] in self?.report($0) }
    }

    @objc func factoryReset() {
        guard FileManager.default.fileExists(atPath: Board.factoryFlash.path) else {
            report(NSError(domain: "Spoke", code: 1, userInfo: [NSLocalizedDescriptionKey:
                "There is no factory flash image (flash.factory.bin) in \(Board.dataDir.path). " +
                "Choose Install OS… in the Model:Cycles menu to install it again."]))
            return
        }
        guard confirm("Factory reset?",
                      detail: "The Model:Cycles shuts down and starts again as it left the factory: an empty +Drive " +
                              "(formatted with its factory content) and factory settings. All projects, samples and " +
                              "settings are erased.",
                      button: "Factory Reset") else { return }
        board.factoryReset { [weak self] in self?.report($0) }
    }

    private func confirm(_ message: String, detail: String, button: String) -> Bool {
        let alert = NSAlert()
        alert.alertStyle = .warning
        alert.messageText = message
        alert.informativeText = detail
        alert.addButton(withTitle: button).hasDestructiveAction = true
        alert.addButton(withTitle: "Cancel").keyEquivalent = "\u{1b}"
        return alert.runModal() == .alertFirstButtonReturn
    }

    private func report(_ error: Error?) {
        guard let error else { return }
        let alert = NSAlert(error: error)
        alert.runModal()
    }
}

@main
struct SpokeApp: App {
    @NSApplicationDelegateAdaptor(AppDelegate.self) var delegate

    var body: some Scene {
        WindowGroup("Vladiktron Spoke") {
            PanelView(board: delegate.board)
                .ignoresSafeArea()
                .frame(minWidth: 750, minHeight: 501)
        }
        .windowStyle(.hiddenTitleBar)
        .windowResizability(.contentMinSize)
        .commands {
            CommandGroup(replacing: .appInfo) { Button("About Vladiktron Spoke") { AppDelegate.showAbout() } }
            CommandGroup(replacing: .help) {
                Button("Licences") { AppDelegate.showLicences() }
                Button("Source Code") { NSWorkspace.shared.open(AppDelegate.source) }
            }
            CommandMenu("Model:Cycles") {
                Button("Install OS…") { delegate.installFirmware() }
                Button("Reveal Data Folder") { delegate.revealDataFolder() }
                Divider()
                Button("Reset +Drive…") { delegate.resetDrive() }
                Button("Factory reset…") { delegate.factoryReset() }
            }
            CommandMenu("USB Audio") { UsbAudioMenu(usb: delegate.board.usbAudio) }
        }
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
