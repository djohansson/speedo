// Renders an SVG to a square PNG with its transparency, through AppKit's own SVG support (no librsvg or ImageMagick):
// scripts/package/make-icons.ps1 builds it (swiftc) and renders the application's icon at each size its .icns and .ico take.
//
//   svgrender <input.svg> <size in pixels> <output.png>
//
// AppKit's renderer applies gradients, clip paths and feGaussianBlur on a plain element, but not a filter on a <use> or
// on a group around one (speedo.svg's shadows are paths for that).
import AppKit

let arguments = CommandLine.arguments
guard arguments.count == 4, let size = Int(arguments[2]), size > 0 else
{
	FileHandle.standardError.write("usage: svgrender <input.svg> <size in pixels> <output.png>\n".data(using: .utf8)!)
	exit(2)
}
guard let image = NSImage(contentsOf: URL(fileURLWithPath: arguments[1])) else
{
	FileHandle.standardError.write("svgrender: can't load \(arguments[1])\n".data(using: .utf8)!)
	exit(1)
}

let bitmap = NSBitmapImageRep(
	bitmapDataPlanes: nil, pixelsWide: size, pixelsHigh: size, bitsPerSample: 8, samplesPerPixel: 4, hasAlpha: true,
	isPlanar: false, colorSpaceName: .deviceRGB, bytesPerRow: 0, bitsPerPixel: 0)!
bitmap.size = NSSize(width: size, height: size)
NSGraphicsContext.saveGraphicsState()
NSGraphicsContext.current = NSGraphicsContext(bitmapImageRep: bitmap)
NSGraphicsContext.current!.imageInterpolation = .high
image.draw(in: NSRect(x: 0, y: 0, width: size, height: size), from: .zero, operation: .copy, fraction: 1)
NSGraphicsContext.restoreGraphicsState()

guard let png = bitmap.representation(using: .png, properties: [:]) else
{
	FileHandle.standardError.write("svgrender: can't encode the PNG\n".data(using: .utf8)!)
	exit(1)
}
do
{
	try png.write(to: URL(fileURLWithPath: arguments[3]))
}
catch
{
	FileHandle.standardError.write("svgrender: \(error)\n".data(using: .utf8)!)
	exit(1)
}
