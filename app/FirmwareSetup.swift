// First run: the OS is not part of the app, so the user points it at their own copy of Elektron's OS 1.13
// (model-cycles_OS1.13.zip from Elektron's support site, or the .syx inside it). The app decodes it into its data
// folder (mcfirmware.h) and boots.
import AppKit
import SwiftUI
import UniformTypeIdentifiers

enum FirmwareFile {
    static let name = "model-cycles_OS1.13.syx"
    static let support = URL(string: "https://www.elektron.se/support-downloads/modelcycles#resources")!

    /// Asks for the .syx (or Elektron's .zip) with an Open panel; nil if the user cancels.
    static func choose() -> URL? {
        let open = NSOpenPanel()
        open.title = "Choose the Model:Cycles OS 1.13 file"
        open.message = "Choose \(name), or the .zip from Elektron it comes in."
        open.prompt = "Install"
        open.allowedContentTypes = [UTType(filenameExtension: "syx") ?? .data, .zip]
        open.allowsMultipleSelection = false
        return open.runModal() == .OK ? open.url : nil
    }
}

struct FirmwareSetupView: View {
    @ObservedObject var board: Board
    @State private var installing = false
    @State private var problem: String?
    @State private var dropTargeted = false

    var body: some View {
        ZStack {
            Color.black.opacity(0.35)
            VStack(alignment: .leading, spacing: 14) {
                Text("Install the Model:Cycles OS").font(.title2.weight(.semibold))
                Text(LocalizedStringKey("Vladiktron Spoke runs Elektron's own OS 1.13 for the Model:Cycles. It doesn't come " +
                     "with the app: download **Model:Cycles OS 1.13** from Elektron's support page, then choose the " +
                     ".zip (or the .syx inside it) here, or drop it on this window."))
                    .fixedSize(horizontal: false, vertical: true)
                Text("The app checks that it is the original OS 1.13 and keeps a decoded copy in its data folder. " +
                     "The file itself is not changed.")
                    .font(.callout).foregroundStyle(.secondary)
                    .fixedSize(horizontal: false, vertical: true)
                if let problem {
                    Label(problem, systemImage: "exclamationmark.triangle.fill")
                        .foregroundStyle(.red)
                        .fixedSize(horizontal: false, vertical: true)
                }
                HStack {
                    Button("Open Elektron Support") {
                        NSWorkspace.shared.open(FirmwareFile.support)
                    }
                    Spacer()
                    if installing { ProgressView().controlSize(.small) }
                    Button("Choose OS File…") { if let url = FirmwareFile.choose() { install(url) } }
                        .keyboardShortcut(.defaultAction)
                        .disabled(installing)
                }
            }
            .padding(24)
            .frame(width: 440)
            .background(RoundedRectangle(cornerRadius: 12).fill(.regularMaterial))
            .overlay(RoundedRectangle(cornerRadius: 12)
                .strokeBorder(Color.accentColor, lineWidth: 3).opacity(dropTargeted ? 1 : 0))
            .shadow(radius: 20)
        }
        .dropDestination(for: URL.self) { urls, _ in
            guard let url = urls.first, !installing else { return false }
            install(url)
            return true
        } isTargeted: { dropTargeted = $0 }
    }

    private func install(_ url: URL) {
        installing = true
        problem = nil
        board.installFirmware(from: url) { error in
            installing = false
            problem = error
        }
    }
}
