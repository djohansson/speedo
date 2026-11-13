// mslcheck: compiles Metal shading language files with Metal's runtime compiler (MTLDevice newLibraryWithSource, MSL 3.2,
// fast math), as a Metal backend compiles slang's output at run time, and prints each file's result and its errors
// (macOS; scripts/shadercheck.ps1 -Target metal -Validate builds and runs it: see docs/backends-plan.md).
//
//   mslcheck <file.metal>...
//
// Xcode's offline compiler (xcrun metal) needs the Metal Toolchain component; this needs only the framework. Built by
// shadercheck.ps1 with the command line tools' clang: clang++ -std=c++20 -fobjc-arc -framework Metal -framework
// Foundation mslcheck.mm -o mslcheck. Exits 1 if any file fails, 2 without a Metal device.
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>

#include <chrono>
#include <cstdio>

int main(int argc, char** argv)
{
	@autoreleasepool
	{
		id<MTLDevice> device = MTLCreateSystemDefaultDevice();
		if (device == nil)
		{
			std::fprintf(stderr, "no Metal device\n");
			return 2;
		}
		std::printf("device: %s\n", device.name.UTF8String);
		MTLCompileOptions* options = [MTLCompileOptions new];
		options.languageVersion = MTLLanguageVersion3_2;
		options.mathMode = MTLMathModeFast;
		int failed = 0;
		for (int argIt = 1; argIt < argc; argIt++)
		{
			NSError* error = nil;
			NSString* path = [NSString stringWithUTF8String:argv[argIt]];
			NSString* source = [NSString stringWithContentsOfFile:path encoding:NSUTF8StringEncoding error:&error];
			if (source == nil)
			{
				std::printf("FAIL %s (unreadable)\n", argv[argIt]);
				failed++;
				continue;
			}
			auto start = std::chrono::steady_clock::now();
			id<MTLLibrary> library = [device newLibraryWithSource:source options:options error:&error];
			double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
			if (library == nil)
			{
				failed++;
				std::printf("FAIL %s (%.0f ms)\n", path.lastPathComponent.UTF8String, ms);
				for (NSString* line in [error.localizedDescription componentsSeparatedByString:@"\n"])
					if ([line containsString:@"error"])
						std::printf("    %s\n", line.UTF8String);
			}
			else
				std::printf("OK %s (%.0f ms, %lu functions)\n", path.lastPathComponent.UTF8String, ms,
					static_cast<unsigned long>(library.functionNames.count));
		}
		std::printf("%d of %d failed\n", failed, argc - 1);
		return failed == 0 ? 0 : 1;
	}
}
