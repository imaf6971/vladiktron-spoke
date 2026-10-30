// Notices (app/Board.swift, app/UsbAudio.swift) and the bar that shows them under the panel (app/PanelView.swift).
import AppKit
import SwiftUI
import XCTest
@testable import VladiktronSpoke

final class NoticeTests: XCTestCase {
    func testDeniedMicrophoneSaysWhereToAllowIt() {
        let n = UsbAudio.microphoneDenied("BlackHole 2ch")
        XCTAssertTrue(n.text.contains("BlackHole 2ch") && n.text.contains("Microphone"), n.text)
        XCTAssertEqual(n.actionTitle, "Open Privacy Settings")
        XCTAssertEqual(n.actionURL?.scheme, "x-apple.systempreferences")
    }

    /// The bar lays out with one row per notice; the picture is saved for a look (TMPDIR/noticebar.png).
    @MainActor
    func testNoticeBarRenders() throws {
        let notices = [
            "main": Notice(text: "No sound: MacBook Pro Speakers would not start (Core Audio error -10851). " +
                                 "It tries again when the output changes."),
            "usb in": UsbAudio.microphoneDenied("BlackHole 2ch"),
        ]
        let view = NSHostingView(rootView: NoticeBar(notices: notices, dismiss: { _ in }).frame(width: 760))
        view.frame.size = view.fittingSize
        XCTAssertGreaterThan(view.frame.height, 60, "two rows should not fit in \(view.frame.height) pt")
        let rep = try XCTUnwrap(view.bitmapImageRepForCachingDisplay(in: view.bounds))
        view.cacheDisplay(in: view.bounds, to: rep)
        let url = FileManager.default.temporaryDirectory.appendingPathComponent("noticebar.png")
        try rep.representation(using: .png, properties: [:])?.write(to: url)
        print("notice bar: \(url.path)")
    }
}
