// Draws the app icon: six pads on spokes around a knob (six tracks, one wheel), in the panel's colours.
//   swift app/make_icon.swift app/AppIcon.icns
// Renders every size an .icns needs into a temporary .iconset and runs iconutil on it.
import AppKit

func rgb(_ r: CGFloat, _ g: CGFloat, _ b: CGFloat, _ a: CGFloat = 1) -> CGColor { CGColor(red: r, green: g, blue: b, alpha: a) }

func gradient(_ c: CGContext, _ colors: [CGColor], from p0: CGPoint, to p1: CGPoint) {
    let g = CGGradient(colorsSpace: CGColorSpace(name: CGColorSpace.sRGB), colors: colors as CFArray, locations: nil)!
    c.drawLinearGradient(g, start: p0, end: p1, options: [])
}

func radial(_ c: CGContext, _ colors: [CGColor], at p: CGPoint, radius: CGFloat) {
    let g = CGGradient(colorsSpace: CGColorSpace(name: CGColorSpace.sRGB), colors: colors as CFArray, locations: nil)!
    c.drawRadialGradient(g, startCenter: p, startRadius: 0, endCenter: p, endRadius: radius, options: [])
}

func circle(_ p: CGPoint, _ r: CGFloat) -> CGPath { CGPath(ellipseIn: CGRect(x: p.x - r, y: p.y - r, width: 2 * r, height: 2 * r), transform: nil) }

// Draws on a 1024-unit canvas (macOS grid: 824-unit body, 100-unit margin, room for the shadow)
func draw(_ c: CGContext) {
    let mid = CGPoint(x: 512, y: 512)
    let body = CGRect(x: 100, y: 100, width: 824, height: 824)
    let bodyPath = CGPath(roundedRect: body, cornerWidth: 185, cornerHeight: 185, transform: nil)

    // Body: the panel's warm aluminium, with a drop shadow
    c.saveGState()
    c.setShadow(offset: CGSize(width: 0, height: -12), blur: 28, color: rgb(0, 0, 0, 0.35))
    c.addPath(bodyPath); c.setFillColor(rgb(0.6, 0.604, 0.588)); c.fillPath()
    c.restoreGState()
    c.saveGState()
    c.addPath(bodyPath); c.clip()
    gradient(c, [rgb(0.70, 0.705, 0.69), rgb(0.53, 0.534, 0.52)], from: CGPoint(x: 0, y: 924), to: CGPoint(x: 0, y: 100))
    // brushed lines
    for i in stride(from: 100, to: 924, by: 6) {
        let y = CGFloat(i)
        c.setStrokeColor(rgb(1, 1, 1, i % 12 == 0 ? 0.035 : 0.0))
        c.setLineWidth(2)
        c.move(to: CGPoint(x: 100, y: y)); c.addLine(to: CGPoint(x: 924, y: y)); c.strokePath()
    }
    c.restoreGState()
    // rim highlight
    c.addPath(CGPath(roundedRect: body.insetBy(dx: 2, dy: 2), cornerWidth: 183, cornerHeight: 183, transform: nil))
    c.setStrokeColor(rgb(1, 1, 1, 0.35)); c.setLineWidth(4); c.strokePath()

    // Six spokes, engraved
    let ring: CGFloat = 268
    let pads = (0..<6).map { i -> CGPoint in
        let a = CGFloat.pi / 2 - CGFloat(i) * .pi / 3
        return CGPoint(x: mid.x + ring * cos(a), y: mid.y + ring * sin(a))
    }
    for p in pads {
        for (dy, color, w) in [(CGFloat(-3), rgb(1, 1, 1, 0.35), CGFloat(16)), (0, rgb(0.16, 0.165, 0.17, 0.85), 12)] {
            c.setStrokeColor(color); c.setLineWidth(w); c.setLineCap(.round)
            c.move(to: CGPoint(x: mid.x, y: mid.y + dy)); c.addLine(to: CGPoint(x: p.x, y: p.y + dy)); c.strokePath()
        }
    }
    // outer ring groove
    c.addPath(circle(mid, ring)); c.setStrokeColor(rgb(0.16, 0.165, 0.17, 0.25)); c.setLineWidth(6); c.strokePath()

    // Pads: grey, the first one lit like a playing trig
    for (i, p) in pads.enumerated() {
        let lit = i == 0
        let r: CGFloat = 76
        if lit {
            c.saveGState(); c.addPath(bodyPath); c.clip(); c.setBlendMode(.screen)
            radial(c, [rgb(1.0, 0.33, 0.28, 0.75), rgb(1.0, 0.33, 0.28, 0)], at: p, radius: 170)
            c.restoreGState()
        }
        c.saveGState()
        c.setShadow(offset: CGSize(width: 0, height: -8), blur: 14, color: rgb(0, 0, 0, 0.45))
        c.addPath(circle(p, r + 8)); c.setFillColor(rgb(0.22, 0.225, 0.235)); c.fillPath()
        c.restoreGState()
        c.saveGState(); c.addPath(circle(p, r)); c.clip()
        if lit {
            gradient(c, [rgb(1.0, 0.66, 0.60), rgb(0.95, 0.40, 0.34)], from: CGPoint(x: p.x, y: p.y + r), to: CGPoint(x: p.x, y: p.y - r))
            radial(c, [rgb(1, 0.95, 0.9, 0.9), rgb(1, 0.6, 0.5, 0)], at: CGPoint(x: p.x, y: p.y + 10), radius: r * 0.8)
        } else {
            gradient(c, [rgb(0.66, 0.67, 0.71), rgb(0.48, 0.495, 0.53)], from: CGPoint(x: p.x, y: p.y + r), to: CGPoint(x: p.x, y: p.y - r))
        }
        c.restoreGState()
        c.addPath(circle(p, r - 2)); c.setStrokeColor(rgb(1, 1, 1, lit ? 0.5 : 0.3)); c.setLineWidth(3); c.strokePath()
    }

    // Hub: the teal knob, seen from above
    let kr: CGFloat = 132
    c.saveGState()
    c.setShadow(offset: CGSize(width: 0, height: -14), blur: 26, color: rgb(0, 0, 0, 0.55))
    c.addPath(circle(mid, kr + 14)); c.setFillColor(rgb(0.10, 0.105, 0.11)); c.fillPath()
    c.restoreGState()
    // knurled skirt
    c.saveGState(); c.addPath(circle(mid, kr + 14)); c.clip()
    for i in 0..<72 {
        let a = CGFloat(i) * .pi / 36
        c.setStrokeColor(rgb(1, 1, 1, i % 2 == 0 ? 0.10 : 0.0)); c.setLineWidth(5)
        c.move(to: CGPoint(x: mid.x + kr * cos(a), y: mid.y + kr * sin(a)))
        c.addLine(to: CGPoint(x: mid.x + (kr + 14) * cos(a), y: mid.y + (kr + 14) * sin(a))); c.strokePath()
    }
    c.restoreGState()
    c.saveGState(); c.addPath(circle(mid, kr)); c.clip()
    gradient(c, [rgb(0.30, 0.70, 0.78), rgb(0.11, 0.42, 0.51)], from: CGPoint(x: mid.x - 60, y: mid.y + kr), to: CGPoint(x: mid.x + 60, y: mid.y - kr))
    radial(c, [rgb(1, 1, 1, 0.35), rgb(1, 1, 1, 0)], at: CGPoint(x: mid.x - 40, y: mid.y + 50), radius: kr)
    c.restoreGState()
    c.addPath(circle(mid, kr - 2)); c.setStrokeColor(rgb(1, 1, 1, 0.35)); c.setLineWidth(3); c.strokePath()
    // pointer, aimed at the lit pad
    c.setStrokeColor(rgb(0.95, 0.97, 0.98)); c.setLineWidth(18); c.setLineCap(.round)
    c.move(to: CGPoint(x: mid.x, y: mid.y + 30)); c.addLine(to: CGPoint(x: mid.x, y: mid.y + kr - 26)); c.strokePath()
}

func png(size: Int) -> Data {
    let rep = NSBitmapImageRep(bitmapDataPlanes: nil, pixelsWide: size, pixelsHigh: size, bitsPerSample: 8, samplesPerPixel: 4,
                               hasAlpha: true, isPlanar: false, colorSpaceName: .deviceRGB, bytesPerRow: 0, bitsPerPixel: 0)!
    let ctx = NSGraphicsContext(bitmapImageRep: rep)!
    let c = ctx.cgContext
    c.scaleBy(x: CGFloat(size) / 1024, y: CGFloat(size) / 1024)
    c.interpolationQuality = .high
    draw(c)
    return rep.representation(using: .png, properties: [:])!
}

let out = CommandLine.arguments.count > 1 ? CommandLine.arguments[1] : "AppIcon.icns"
let set = URL(fileURLWithPath: NSTemporaryDirectory()).appendingPathComponent("AppIcon-\(getpid()).iconset")
try FileManager.default.createDirectory(at: set, withIntermediateDirectories: true)
for base in [16, 32, 128, 256, 512] {
    try png(size: base).write(to: set.appendingPathComponent("icon_\(base)x\(base).png"))
    try png(size: base * 2).write(to: set.appendingPathComponent("icon_\(base)x\(base)@2x.png"))
}
let p = Process()
p.executableURL = URL(fileURLWithPath: "/usr/bin/iconutil")
p.arguments = ["-c", "icns", set.path, "-o", out]
try p.run(); p.waitUntilExit()
try? FileManager.default.removeItem(at: set)
exit(p.terminationStatus)
