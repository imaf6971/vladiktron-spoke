// The Settings window (Vladiktron Spoke → Settings…, ⌘,): where the Model:Cycles' sound goes, its USB audio routes
// (also in the USB Audio menu) and its data folder, with the Model:Cycles menu's commands for it.
import AppKit
import SwiftUI

struct SettingsView: View {
    @ObservedObject var board: Board
    @ObservedObject var usb: UsbAudio
    let app: AppDelegate                                // the Model:Cycles menu's commands

    var body: some View {
        Form {
            Section {
                Picker("Main output", selection: $board.mainOutputUID) {
                    Text("System default").tag(String?.none)
                    if let uid = board.mainOutputUID, !usb.outputs.contains(where: { $0.uid == uid }) {
                        Text("Not connected (plays on the default)").tag(Optional(uid))
                    }
                    ForEach(usb.outputs) { Text($0.name).tag(Optional($0.uid)) }
                }
            } header: {
                Text("Sound")
            } footer: {
                Text("Where the Model:Cycles' main and headphone outputs play.").foregroundStyle(.secondary)
                    .multilineTextAlignment(.leading).frame(maxWidth: .infinity, alignment: .leading)
            }
            Section {
                UsbAudioMenu(usb: usb)
            } header: {
                Text("USB Audio")
            } footer: {
                Text("Over USB the Model:Cycles is an audio interface. To record it in a DAW, or play into it, " +
                     "route it to a loopback device such as BlackHole.").foregroundStyle(.secondary)
                    .multilineTextAlignment(.leading).frame(maxWidth: .infinity, alignment: .leading)
            }
            Section {
                LabeledContent("Folder") {
                    Text(Board.dataDir.path).textSelection(.enabled).lineLimit(1).truncationMode(.middle)
                }
                HStack {
                    Button("Install OS…") { app.installFirmware() }
                    Spacer()
                    Button("Reveal in Finder") { app.revealDataFolder() }
                }
                HStack {
                    Spacer()
                    Button("Reset +Drive…") { app.resetDrive() }
                    Button("Factory reset…") { app.factoryReset() }
                }
            } header: {
                Text("Data")
            } footer: {
                Text("Reset +Drive erases projects, presets and samples; Factory reset also erases settings.")
                    .foregroundStyle(.secondary)
                    .multilineTextAlignment(.leading).frame(maxWidth: .infinity, alignment: .leading)
            }
        }
        .formStyle(.grouped)
        .frame(width: 480)
        .fixedSize(horizontal: false, vertical: true)
    }
}
