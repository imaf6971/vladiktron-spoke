// The Model:Cycles front panel, laid out from a photo of the hardware (design units = photo pixels / 2).
// Keys and pads press while held (⌥-click latches a key so it stays held for combinations); knobs turn by
// dragging up/down or scrolling. Shift = FUNC, Space = PLAY.
import AppKit
import SwiftUI

// MARK: - Hardware map (key-matrix indexes, encoder numbers and LED bits, from the OS tables)

enum Panel {
    static let size = CGSize(width: 1000, height: 668)

    // key-matrix index (mcboard.h)
    static let presetMenu = 0, quantize = 1, click = 2, lfoSetup = 3, tapBpm = 4, stop = 5, tempSave = 6
    static let play = 7, padMenu = 8, record = 9, function = 10, pattern = 11, retrig = 12, track = 13, page = 30
    static func step(_ n: Int) -> Int { 13 + n }       // steps 1..16 -> 14..29

    // LED bits (OS table 0x4010abf8 and observed behaviour)
    static let stepLED = [6, 19, 20, 21, 22, 23, 16, 28, 29, 31, 37, 38, 44, 46, 47, 40]
    static let padLED = [17, 18, 30, 36, 39, 45]
    static let pageLED = [52, 48, 49, 50]
    static let knobLED = [26, 34, 32, 41, 27, 24, 33, 42, 15, 25, 35, 43]     // encoders 2..13
    static let keyLED: [Int: Int] = [play: 0, stop: 12, record: 3, click: 8, quantize: 9, function: 4,
                                     pattern: 5, retrig: 13, track: 7, page: 51, presetMenu: 10,
                                     lfoSetup: 14, padMenu: 11, tempSave: 2, tapBpm: 1]
}

private enum Palette {
    static let body = Color(red: 0.600, green: 0.604, blue: 0.588)
    static let plate = Color(red: 0.573, green: 0.577, blue: 0.561)
    static let label = Color(red: 0.16, green: 0.165, blue: 0.17)
    static let label2 = Color(red: 0.42, green: 0.425, blue: 0.42)
    static let keyEdge = Color(red: 0.22, green: 0.225, blue: 0.235)
    static let keyTop = Color(red: 0.43, green: 0.435, blue: 0.455)
    static let keyBottom = Color(red: 0.34, green: 0.345, blue: 0.365)
    static let keyText = Color(red: 0.86, green: 0.865, blue: 0.875)
    static let litTop = Color(red: 1.0, green: 0.66, blue: 0.60)
    static let litBottom = Color(red: 0.95, green: 0.46, blue: 0.40)
    static let padTop = Color(red: 0.66, green: 0.67, blue: 0.71)
    static let padBottom = Color(red: 0.50, green: 0.515, blue: 0.55)
    static let ledOff = Color(red: 0.13, green: 0.135, blue: 0.14)
    static let ledRed = Color(red: 1.0, green: 0.33, blue: 0.28)
    static let ledYellow = Color(red: 1.0, green: 0.86, blue: 0.45)
}

private func labelFont(_ size: CGFloat) -> Font { .system(size: size, weight: .semibold) }

// MARK: - Brand

// "vladiktron" wordmark with a stylized V, pressed into the body like the maker's logo on the hardware.
private struct BrandMark: View {
    private static let ink = Color(red: 0.47, green: 0.475, blue: 0.465)

    var body: some View {
        HStack(alignment: .center, spacing: 6) {
            VMark().stroke(Self.ink, style: StrokeStyle(lineWidth: 2.2, lineJoin: .round))
                .frame(width: 28, height: 22)
            Text("vladiktron").font(.system(size: 25, weight: .heavy)).italic().tracking(0.4)
                .foregroundStyle(Self.ink)
        }
        .shadow(color: .white.opacity(0.35), radius: 0, y: 1)     // lit lower edge of the engraving
        .shadow(color: .black.opacity(0.18), radius: 0, y: -0.6)
        .fixedSize()
    }
}

// Outlined, forward-leaning V with a notch cut into its point.
private struct VMark: Shape {
    func path(in r: CGRect) -> Path {
        let h = r.height, lean = h * 0.25, w = r.width - lean
        func p(_ x: CGFloat, _ y: CGFloat) -> CGPoint { CGPoint(x: r.minX + x * w + lean * (1 - y), y: r.minY + y * h) }
        var path = Path()
        path.addLines([p(0, 0), p(0.32, 0), p(0.5, 0.6), p(0.68, 0), p(1, 0), p(0.62, 1), p(0.38, 1)])
        path.closeSubpath()
        return path
    }
}

// MARK: - Panel

struct PanelView: View {
    @ObservedObject var board: Board
    @State private var latched: Set<Int> = []
    @State private var hoveredKnob: Int?

    private func lit(_ bit: Int) -> Bool { (board.leds >> UInt64(bit)) & 1 == 1 }

    var body: some View {
        GeometryReader { geo in
            let s = min(geo.size.width / Panel.size.width, geo.size.height / Panel.size.height)
            panel
                .frame(width: Panel.size.width, height: Panel.size.height)
                .scaleEffect(s)
                .position(x: geo.size.width / 2, y: geo.size.height / 2)
        }
        .background(Color(red: 0.600, green: 0.604, blue: 0.588))
        .background(KeyboardMonitor(board: board, hoveredKnob: $hoveredKnob))
        .overlay { if board.needsFirmware { FirmwareSetupView(board: board) } }
    }

    private var panel: some View {
        ZStack(alignment: .topLeading) {
            // body and the recessed control plate
            RoundedRectangle(cornerRadius: 34, style: .continuous)
                .fill(LinearGradient(colors: [Palette.body.opacity(0.98), Palette.body], startPoint: .top, endPoint: .bottom))
                .frame(width: 1000, height: 668)
            RoundedRectangle(cornerRadius: 40, style: .continuous)
                .fill(Palette.plate)
                .overlay(RoundedRectangle(cornerRadius: 40, style: .continuous)
                    .stroke(LinearGradient(colors: [.black.opacity(0.16), .white.opacity(0.22)], startPoint: .top, endPoint: .bottom), lineWidth: 2.5))
                .frame(width: 935, height: 503)
                .position(x: 500, y: 383.5)

            BrandMark().position(x: 836, y: 78)
            group(screenArea)
            group(leftKeys)
            group(iconColumn)
            group(knobs)
            group(rightColumn)
            group(padsAndSteps)
            if !board.needsFirmware { connections.position(x: 500, y: 651) }
        }
        .frame(width: 1000, height: 668, alignment: .topLeading)
    }

    /// Bottom edge: what the virtual cables are plugged into.
    private var connections: some View {
        let usb = board.usbState == 2 ? "USB: connected" : board.usbState < 0 ? "USB: no answer" : "USB: connecting…"
        let ctl = board.controller.map { "  ·  \($0)" } ?? ""
        return Text(usb + ctl).font(labelFont(8)).foregroundStyle(Palette.label2)
    }

    private func group<V: View>(_ v: V) -> some View { v.frame(width: 1000, height: 668, alignment: .topLeading) }

    // MARK: screen + LEVEL/DATA

    private var screenArea: some View {
        ZStack(alignment: .topLeading) {
            KnobView(id: 1, diameter: 55, colors: [Color(red: 0.25, green: 0.62, blue: 0.70), Color(red: 0.13, green: 0.47, blue: 0.56)],
                     board: board, hovered: $hoveredKnob)
                .position(x: 79, y: 181)
            panelLabel("LEVEL / DATA", y: 216.5, x: 80)
            subLabel("TRIG / PAN", y: 228, x: 80)

            ZStack {
                RoundedRectangle(cornerRadius: 7).fill(Color(red: 0.10, green: 0.105, blue: 0.11))
                RoundedRectangle(cornerRadius: 3).fill(Color(red: 0.84, green: 0.855, blue: 0.88))
                    .padding(5)
                if let img = board.screen {
                    Image(decorative: img, scale: 1).interpolation(.none).resizable()
                        .padding(EdgeInsets(top: 6, leading: 6, bottom: 6, trailing: 6))
                } else if let e = board.error {
                    Text(e).font(.system(size: 6)).foregroundStyle(.black).padding(8)
                }
            }
            .frame(width: 129, height: 73)
            .shadow(color: .black.opacity(0.25), radius: 1.5, y: 1)
            .position(x: 203.5, y: 197.5)
            Text("Spoke").font(.system(size: 13.5, weight: .semibold)).foregroundStyle(Palette.label)
                .position(x: 203, y: 252.5)
        }
    }

    // MARK: left key block

    private var leftKeys: some View {
        ZStack(alignment: .topLeading) {
            iconKey(Panel.padMenu, symbol: "arrow.uturn.backward", x: 70, y: 300)
            iconKey(Panel.tempSave, symbol: "wrench.adjustable", x: 159, y: 300)
            iconKey(Panel.tapBpm, symbol: "metronome", x: 247.5, y: 300)
            subLabel("PAD MENU", y: 330, x: 70, underline: true)
            subLabel("TEMP SAVE", y: 330, x: 159, underline: true)
            subLabel("TAP BPM", y: 330, x: 247.5, underline: true)

            iconKey(Panel.record, symbol: "circle", x: 70, y: 374)
            iconKey(Panel.play, symbol: "play", x: 159, y: 374)
            iconKey(Panel.stop, symbol: "square", x: 247.5, y: 374)
            subLabel("COPY", y: 405, x: 70, underline: true)
            subLabel("CLEAR", y: 405, x: 159, underline: true)
            subLabel("PASTE", y: 405, x: 247.5, underline: true)

            textKey(Panel.function, "FUNC", x: 81.5, y: 448.5, underlineText: true)
            textKey(Panel.retrig, "RETRIG", x: 235.5, y: 448.5)
            subLabel("RETRIG MENU", y: 479, x: 235.5, underline: true)
            textKey(Panel.pattern, "PATTERN", x: 81.5, y: 522.5)
            textKey(Panel.track, "TRACK", x: 235.5, y: 522.5)
            subLabel("RELOAD PTN", y: 553.5, x: 81.5, underline: true)
            subLabel("CTRL ALL / TRK MENU", y: 553.5, x: 235.5, underline: true)
        }
    }

    private var iconColumn: some View {
        ZStack(alignment: .topLeading) {
            iconKey(Panel.presetMenu, symbol: "cylinder", x: 336.5, y: 181)
            subLabel("PRESET MENU", y: 212.5, x: 336.5, underline: true)
            iconKey(Panel.quantize, symbol: "cube", x: 336.5, y: 258.5)
            subLabel("QUANTIZE", y: 289, x: 336.5, underline: true)
            iconKey(Panel.click, symbol: "hand.point.up.left", x: 336.5, y: 335)
            subLabel("CLICK", y: 365.5, x: 336.5, underline: true)
            iconKey(Panel.lfoSetup, symbol: "", x: 336.5, y: 411.5, sine: true)
            subLabel("LFO SETUP", y: 442.5, x: 336.5, underline: true)
        }
    }

    // MARK: knobs

    private struct KnobSpec { let id: Int; let name: String; let sub: String?; let x: CGFloat; let y: CGFloat }
    private let grid: [KnobSpec] = [
        .init(id: 2, name: "PITCH", sub: nil, x: 455, y: 181), .init(id: 3, name: "DECAY", sub: nil, x: 570.5, y: 181),
        .init(id: 4, name: "COLOR", sub: nil, x: 686.5, y: 181), .init(id: 5, name: "SHAPE", sub: nil, x: 802, y: 181),
        .init(id: 6, name: "SWEEP", sub: nil, x: 455, y: 296), .init(id: 7, name: "CONTOUR", sub: nil, x: 570.5, y: 296),
        .init(id: 8, name: "DELAY SEND", sub: nil, x: 686.5, y: 296), .init(id: 9, name: "REVERB SEND", sub: nil, x: 802, y: 296),
        .init(id: 10, name: "LFO SPEED", sub: nil, x: 455, y: 411), .init(id: 11, name: "VOLUME + DIST", sub: nil, x: 570.5, y: 411),
        .init(id: 12, name: "SWING", sub: "NUDGE", x: 686.5, y: 411), .init(id: 13, name: "CHANCE", sub: "COND", x: 802, y: 411),
    ]

    private var knobs: some View {
        ZStack(alignment: .topLeading) {
            ForEach(grid, id: \.id) { k in
                KnobView(id: k.id, diameter: 50, colors: [Color(red: 0.42, green: 0.425, blue: 0.445), Color(red: 0.30, green: 0.305, blue: 0.325)],
                         board: board, hovered: $hoveredKnob)
                    .position(x: k.x, y: k.y)
                LED(on: lit(Panel.knobLED[k.id - 2]), color: Palette.ledRed, size: 10)
                    .position(x: k.x - 40.5, y: k.y - 8.5)
                knobLabel(k.name, sub: k.sub).position(x: k.x, y: k.y + 36)
            }
        }
    }

    private func knobLabel(_ name: String, sub: String?) -> some View {
        HStack(spacing: 2) {
            Text(name).foregroundStyle(Palette.label)
            if let sub {
                Text("/").foregroundStyle(Palette.label2)
                Text(sub).foregroundStyle(Palette.label2)
            }
        }
        .font(labelFont(8.6)).fontWidth(.condensed)
        .fixedSize()
    }

    // MARK: right column

    private var rightColumn: some View {
        ZStack(alignment: .topLeading) {
            Image(systemName: "power").font(.system(size: 10, weight: .regular)).foregroundStyle(Palette.label2)
                .position(x: 876.5, y: 172.5)
            KnobView(id: 16, diameter: 50, colors: [Color(white: 0.97), Color(white: 0.86)], board: board, hovered: $hoveredKnob)
                .position(x: 918, y: 181)
            panelLabel("MAIN VOLUME", y: 217, x: 918)
            KnobView(id: 14, diameter: 50, colors: [Color(red: 0.78, green: 0.70, blue: 0.70), Color(red: 0.64, green: 0.55, blue: 0.56)],
                     board: board, hovered: $hoveredKnob)
                .position(x: 918, y: 296)
            panelLabel("REVERB SIZE", y: 332, x: 918)
            subLabel("REV TONE", y: 344, x: 918, underline: true)
            KnobView(id: 15, diameter: 50, colors: [Color(red: 0.78, green: 0.70, blue: 0.70), Color(red: 0.64, green: 0.55, blue: 0.56)],
                     board: board, hovered: $hoveredKnob)
                .position(x: 918, y: 411)
            panelLabel("DELAY TIME", y: 447, x: 918)
            subLabel("DEL FEEDBACK", y: 458.5, x: 918, underline: true)

            ForEach(0..<4, id: \.self) { i in
                let x: CGFloat = [891, 909, 926.5, 944][i]
                LED(on: lit(Panel.pageLED[i]), color: Palette.ledYellow, size: 9.5).position(x: x, y: 478.5)
                Text("\(i + 1):4").font(labelFont(7.5)).fontWidth(.condensed).foregroundStyle(Palette.label2)
                    .position(x: x, y: 491)
            }
            textKey(Panel.page, "PAGE", x: 918, y: 522.5)
            subLabel("FILL / SCALE", y: 553.5, x: 918, underline: true)
        }
    }

    // MARK: pads and steps

    private let padX: [CGFloat] = [336.5, 433.5, 530.5, 627.5, 724, 821]
    private let banks = ["A", "B", "C", "D", "E", "F"]

    private var padsAndSteps: some View {
        ZStack(alignment: .topLeading) {
            ForEach(0..<6, id: \.self) { i in
                PadView(number: i + 1, lit: lit(Panel.padLED[i]), board: board)
                    .position(x: padX[i], y: 511)
                subLabel("BANK \(banks[i]) / MUTE", y: 553.5, x: padX[i], underline: true)
            }
            ForEach(1...16, id: \.self) { n in
                stepKey(n).position(x: 70 + CGFloat(n - 1) * 57.33, y: 597.5)
            }
        }
    }

    private func stepKey(_ n: Int) -> some View {
        KeyView(lit: lit(Panel.stepLED[n - 1]), width: 40, height: 40, radius: 8) {
            VStack(spacing: 0) {
                Text("\(n)").font(.system(size: 15, weight: .semibold)).fontWidth(.condensed)
                Rectangle().frame(width: 11, height: 1.3).opacity((n - 1) % 4 == 0 ? 1 : 0)
            }
            .offset(y: 1)
        } onPress: { down in press(Panel.step(n), down) }
    }

    // MARK: building blocks

    private func press(_ key: Int, _ down: Bool) {
        if down, NSEvent.modifierFlags.contains(.option) {           // ⌥-click: latch / unlatch
            if latched.remove(key) != nil { board.key(key, down: false) } else { latched.insert(key); board.key(key, down: true) }
            return
        }
        if latched.contains(key) { return }
        board.key(key, down: down)
    }

    private func iconKey(_ key: Int, symbol: String, x: CGFloat, y: CGFloat, sine: Bool = false) -> some View {
        KeyView(lit: lit(Panel.keyLED[key] ?? -1), latched: latched.contains(key), width: 40, height: 40, radius: 9) {
            if sine {
                SineIcon().stroke(style: StrokeStyle(lineWidth: 2, lineCap: .round)).frame(width: 17, height: 9)
            } else {
                Image(systemName: symbol).font(.system(size: 15, weight: .medium))
            }
        } onPress: { down in press(key, down) }
        .position(x: x, y: y)
    }

    private func textKey(_ key: Int, _ title: String, x: CGFloat, y: CGFloat, underlineText: Bool = false) -> some View {
        KeyView(lit: lit(Panel.keyLED[key] ?? -1), latched: latched.contains(key), width: 62.5, height: 37.5, radius: 9) {
            Text(title).font(.system(size: 11.5, weight: .bold)).fontWidth(.condensed).underline(underlineText)
        } onPress: { down in press(key, down) }
        .position(x: x, y: y)
    }

    private func panelLabel(_ t: String, y: CGFloat, x: CGFloat) -> some View {
        Text(t).font(labelFont(8.6)).fontWidth(.condensed).foregroundStyle(Palette.label).fixedSize().position(x: x, y: y)
    }

    private func subLabel(_ t: String, y: CGFloat, x: CGFloat, underline: Bool = false) -> some View {
        Text(t).font(labelFont(8.4)).fontWidth(.condensed).foregroundStyle(Palette.label2).underline(underline)
            .fixedSize().position(x: x, y: y)
    }
}

// MARK: - Controls

struct KeyView<Content: View>: View {
    var lit: Bool
    var latched: Bool = false
    var width: CGFloat
    var height: CGFloat
    var radius: CGFloat
    @ViewBuilder var content: () -> Content
    var onPress: (Bool) -> Void
    @State private var down = false

    var body: some View {
        let pressed = down || latched
        ZStack {
            RoundedRectangle(cornerRadius: radius + 1.5, style: .continuous)
                .fill(Palette.keyEdge)
                .shadow(color: .black.opacity(0.35), radius: pressed ? 0.5 : 1.6, y: pressed ? 0.5 : 1.6)
            RoundedRectangle(cornerRadius: radius, style: .continuous)
                .fill(LinearGradient(colors: lit ? [Palette.litTop, Palette.litBottom] : [Palette.keyTop, Palette.keyBottom],
                                     startPoint: .top, endPoint: .bottom))
                .padding(2.6)
                .shadow(color: lit ? Palette.litBottom.opacity(0.6) : .clear, radius: 4)
            content()
                .foregroundStyle(lit ? Color.white.opacity(0.92) : Palette.keyText)
        }
        .frame(width: width, height: height)
        .scaleEffect(pressed ? 0.965 : 1)
        .brightness(pressed ? -0.04 : 0)
        .contentShape(Rectangle())
        .gesture(DragGesture(minimumDistance: 0)
            .onChanged { _ in if !down { down = true; onPress(true) } }
            .onEnded { _ in down = false; onPress(false) })
        .animation(.easeOut(duration: 0.06), value: pressed)
    }
}

struct PadView: View {
    let number: Int
    let lit: Bool
    let board: Board
    @State private var down = false

    var body: some View {
        ZStack {
            RoundedRectangle(cornerRadius: 11, style: .continuous).fill(Palette.keyEdge)
                .shadow(color: .black.opacity(0.35), radius: down ? 0.5 : 2, y: down ? 0.5 : 2)
            RoundedRectangle(cornerRadius: 9, style: .continuous)
                .fill(LinearGradient(colors: lit ? [Palette.litTop, Palette.litBottom] : [Palette.padTop, Palette.padBottom],
                                     startPoint: .top, endPoint: .bottom))
                .padding(3)
                .shadow(color: lit ? Palette.litBottom.opacity(0.7) : .clear, radius: 6)
            RoundedRectangle(cornerRadius: 6, style: .continuous)
                .fill(RadialGradient(colors: [.white.opacity(lit ? 0.35 : 0.18), .clear], center: .center, startRadius: 2, endRadius: 30))
                .padding(9)
            Text("T\(number)").font(.system(size: 15, weight: .bold)).fontWidth(.condensed)
                .foregroundStyle(lit ? Color.white.opacity(0.85) : Color(white: 0.9))
        }
        .frame(width: 62.5, height: 62.5)
        .scaleEffect(down ? 0.97 : 1)
        .contentShape(Rectangle())
        .gesture(DragGesture(minimumDistance: 0)
            .onChanged { v in
                // pressure from where the pad is hit: centre = hardest, like a real pad hit square-on
                let dx = abs(v.startLocation.x - 31.25) / 31.25, dy = abs(v.startLocation.y - 31.25) / 31.25
                let p = Float(max(0.45, 1.0 - 0.5 * max(dx, dy)))
                if !down { down = true }
                board.pad(number, pressure: p)
            }
            .onEnded { _ in down = false; board.pad(number, pressure: 0) })
    }
}

struct KnobView: View {
    let id: Int
    let diameter: CGFloat
    let colors: [Color]
    let board: Board
    @Binding var hovered: Int?
    @State private var lastY: CGFloat?
    @State private var residue: CGFloat = 0
    @State private var spin: Double = 0

    var body: some View {
        ZStack {
            Circle().fill(Color.black.opacity(0.28)).blur(radius: 2.5).offset(y: 2.5)
            Circle().fill(LinearGradient(colors: colors, startPoint: .top, endPoint: .bottom))
            Circle().fill(RadialGradient(colors: [.white.opacity(0.16), .clear], center: UnitPoint(x: 0.4, y: 0.32),
                                         startRadius: 1, endRadius: diameter * 0.5))
            Circle().stroke(Color.black.opacity(0.22), lineWidth: 1.2)
            // faint grip texture so a turn is visible on the endless encoder
            ForEach(0..<24, id: \.self) { i in
                Capsule().fill(Color.black.opacity(0.07)).frame(width: 1.2, height: diameter * 0.09)
                    .offset(y: -diameter * 0.43).rotationEffect(.degrees(Double(i) * 15 + spin))
            }
        }
        .frame(width: diameter, height: diameter)
        .contentShape(Circle())
        .onHover { inside in if inside { hovered = id } else if hovered == id { hovered = nil } }
        .gesture(DragGesture(minimumDistance: 0)
            .onChanged { v in
                let y = v.location.y - v.location.x * 0.35
                defer { lastY = y }
                guard let ly = lastY else { return }
                residue += ly - y                      // up = clockwise
                let steps = Int(residue / 5)           // 5 pt of drag per detent
                if steps != 0 { residue -= CGFloat(steps) * 5; turn(steps) }
            }
            .onEnded { _ in lastY = nil; residue = 0 })
        .onReceive(NotificationCenter.default.publisher(for: .knobScroll)) { n in
            guard let (knob, steps) = n.object as? (Int, Int), knob == id else { return }
            turn(steps)
        }
    }

    private func turn(_ steps: Int) {
        board.turn(id, steps: steps)
        spin += Double(steps) * 6
    }
}

struct LED: View {
    let on: Bool
    let color: Color
    let size: CGFloat
    var body: some View {
        RoundedRectangle(cornerRadius: 1.5)
            .fill(on ? color : Palette.ledOff)
            .frame(width: size, height: size)
            .shadow(color: on ? color.opacity(0.9) : .clear, radius: 3)
            .overlay(RoundedRectangle(cornerRadius: 1.5).stroke(Color.black.opacity(0.35), lineWidth: 0.6))
    }
}

struct SineIcon: Shape {
    func path(in r: CGRect) -> Path {
        var p = Path()
        for i in 0...40 {
            let t = CGFloat(i) / 40
            let pt = CGPoint(x: r.minX + t * r.width, y: r.midY - sin(t * 2 * .pi) * r.height / 2)
            if i == 0 { p.move(to: pt) } else { p.addLine(to: pt) }
        }
        return p
    }
}

extension Notification.Name { static let knobScroll = Notification.Name("knobScroll") }

// MARK: - Keyboard and scroll wheel

/// Shift = FUNC, Space = PLAY; the scroll wheel turns the knob under the pointer.
struct KeyboardMonitor: NSViewRepresentable {
    let board: Board
    @Binding var hoveredKnob: Int?

    final class Coordinator {
        var monitors: [Any] = []
        var shift = false
        var space = false
        var scrollResidue: CGFloat = 0
    }

    func makeCoordinator() -> Coordinator { Coordinator() }

    func makeNSView(context: Context) -> NSView {
        let c = context.coordinator
        let board = self.board
        let hovered = $hoveredKnob
        c.monitors.append(NSEvent.addLocalMonitorForEvents(matching: .flagsChanged) { e in
            let s = e.modifierFlags.contains(.shift)
            if s != c.shift { c.shift = s; board.key(Panel.function, down: s) }
            return e
        } as Any)
        c.monitors.append(NSEvent.addLocalMonitorForEvents(matching: [.keyDown, .keyUp]) { e in
            guard e.keyCode == 49 else { return e }                    // space
            let down = e.type == .keyDown
            if down != c.space { c.space = down; board.key(Panel.play, down: down) }
            return nil
        } as Any)
        c.monitors.append(NSEvent.addLocalMonitorForEvents(matching: .scrollWheel) { e in
            guard let knob = hovered.wrappedValue else { return e }
            c.scrollResidue += e.scrollingDeltaY * (e.hasPreciseScrollingDeltas ? 0.15 : 1)
            let steps = Int(c.scrollResidue)
            if steps != 0 {
                c.scrollResidue -= CGFloat(steps)
                NotificationCenter.default.post(name: .knobScroll, object: (knob, steps))
            }
            return nil
        } as Any)
        return NSView()
    }

    func updateNSView(_ nsView: NSView, context: Context) {}

    static func dismantleNSView(_ nsView: NSView, coordinator: Coordinator) {
        coordinator.monitors.forEach { NSEvent.removeMonitor($0) }
    }
}
