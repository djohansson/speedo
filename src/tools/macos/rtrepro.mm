// rtrepro: Metal 4 ray queries under contention between processes, without Vulkan (macOS; see CLAUDE.md, "Hardware ray
// queries": KosmicKrisp's ray queries beside other processes' gpu work restarted the gpu).
//
//   rtrepro trace <seconds> [rays file] [threads] [rays per thread]
//   rtrepro burn <seconds> [threads] [iterations]
//   rtrepro hog <seconds> <gigabytes>
//   rtrepro present <seconds>
//   rtrepro stamp <seconds> [threads] [iterations]
//   rtrepro compile <seconds> [dispatch: 1 or 0] [compiler: mtl4 or device] [queue: mtl4 or classic]
//   rtrepro frame <seconds> [flags, rtpudx by default]
//
// frame: a speedo frame's shape around its ray queries, in a window of 1280x720 points behind the others: a pre-pass
// render (a full screen triangle into a color and a depth target), a compute dispatch of a thread per pixel of 1280x720
// (groups of 8x8) that reads the pre-pass's targets and writes a texture, with a ray against a unit cube's instance
// structure per pixel, then a render pass sampling that texture into the drawable, every refresh; Metal 4 (an
// MTL4CommandQueue, producer barriers after each encoder as KosmicKrisp places them). Its flags, each a letter: r the
// kernel compiled with ray tracing (an intersection_query; i an intersector's one call instead), t it traces (else the
// query never runs), p the pre-pass, u the pass into the drawable, d presenting (else into a target of its own at about
// the display's rate), x the kernel's reads of the pre-pass, b burn's load in this process on a queue of its own, m the
// classic queue instead (an MTLCommandQueue with automatic hazard tracking and presentDrawable), h the window off every
// screen, c the compute in a command buffer of its own, s a counter heap resolved after each render pass, 0 the
// kernels as they first restarted the gpu. With r and load (b, or burn or present in other processes), the gpu restarts
// within seconds (CDM Kill timeout); without r, never: `rtrepro frame 60 rtpudxb` alone restarted it 3 runs in 3.
//
// trace: three unit cubes in a primitive acceleration structure each (built once), an instance structure per frame in
// flight over them rebuilt every frame (indirect instance descriptors), then a compute dispatch whose threads each trace
// rays per thread rays with intersection_query<instancing, triangle_data> (opaque geometry, bounding boxes culled, as
// KosmicKrisp lowers a Vulkan ray query), short visibility rays read from the file (a ray a line: origin xyz, direction
// xyz, tmax; more numbers ignored; # comments; rtrepro-rays.txt beside this file holds the rays a crash report's ray
// query watch had in flight). Metal 4 throughout: an MTL4CommandQueue, command allocators per frame in flight, an argument
// table, a residency set. burn: a compute dispatch of an ALU loop a frame, no ray tracing, for contention. hog: what a
// speedo client on KosmicKrisp does to memory, without its rendering: that many gigabytes of shared buffers touched and
// in its residency set, committed and made resident again before every frame (as KosmicKrisp does at every submit),
// and a small dispatch a frame at about 120 a second. present: what a speedo client's window does, without its rendering:
// a window of 1280x720 points behind the others (as speedo's background windows), a frame cleared and presented every
// refresh through a CAMetalLayer, by Metal 4's drawable flow (waitForDrawable, signalDrawable, as KosmicKrisp presents),
// which WindowServer composites. stamp: burn's work in 16 dispatches a frame, each between timestamps written into a
// Metal 4 counter heap (relaxed granularity, in the compute encoder, as KosmicKrisp writes a Vulkan timestamp query and
// speedo's profile builds write two around every pass), resolved every frame. compile: what a speedo client does in its
// first seconds, creating its pipelines: kernels of unique source (never a cache hit) compiled one after another
// through an MTL4Compiler (a library from MSL source, then a compute pipeline, as KosmicKrisp compiles every pipeline),
// each dispatched once as soon as it is made (not with a dispatch of 0; with the compiler `device`, through MTLDevice's
// newLibraryWithSource and newComputePipelineStateWithFunction instead; with the queue `classic`, dispatched through an
// MTLCommandQueue rather than an MTL4CommandQueue). All print frames and errors (commit feedback) once a second, and stop at the first
// error or a frame not done within 10 s.
// contend.ps1 runs several at once and stops them at the first gpu restart.
//
// not part of the build (CMake has no Objective-C++ here): build it with Apple's clang,
//   xcrun clang++ -std=c++20 -fobjc-arc -O2 -Wall -framework Metal -framework Foundation -framework AppKit \
//     -framework QuartzCore rtrepro.mm -o rtrepro
#import <AppKit/AppKit.h>
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#import <QuartzCore/CAMetalLayer.h>

#include <atomic>
#include <cmath>
#include <unistd.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

static const char* kSource = R"(
#include <metal_stdlib>
#include <metal_raytracing>
using namespace metal;
using namespace raytracing;

struct TraceParams
{
	uint rayCount;
	uint raysPerThread;
	uint frame;
};

kernel void trace(
	device const float4* rays [[buffer(0)]],
	device atomic_uint* counters [[buffer(1)]],
	constant TraceParams& params [[buffer(2)]],
	instance_acceleration_structure structure [[buffer(3)]],
	uint tid [[thread_position_in_grid]])
{
	uint hits = 0;
	for (uint rayIt = 0; rayIt < params.raysPerThread; rayIt++)
	{
		uint index = (tid * params.raysPerThread + rayIt + params.frame) % params.rayCount;
		float4 originTmax = rays[index * 2];
		float4 direction = rays[index * 2 + 1];
		intersection_params intersectionParams;
		intersectionParams.set_geometry_cull_mode(geometry_cull_mode::bounding_box);
		intersection_query<instancing, triangle_data> query;
		query.reset(ray(originTmax.xyz, direction.xyz, 0.0, originTmax.w), structure, 0xFF, intersectionParams);
		while (query.next())
		{
		}
		if (query.get_committed_intersection_type() == intersection_type::triangle)
			hits++;
	}
	atomic_fetch_add_explicit(&counters[0], hits, memory_order_relaxed);
}

struct BurnParams
{
	uint iterations;
};

kernel void burn(device float* out [[buffer(0)]], constant BurnParams& params [[buffer(1)]], uint tid [[thread_position_in_grid]])
{
	float x = float(tid);
	for (uint it = 0; it < params.iterations; it++)
		x = fma(x, 1.000001, 0.5);
	if (x == 12345.0)
		out[tid & 1023] = x;
}
)";

static constexpr uint32_t kFramesInFlight = 3;
static constexpr uint32_t kInstanceCount = 3;

static std::atomic<int> gErrors{0};

static MTL4BufferRange Range(id<MTLBuffer> buffer, uint64_t offset = 0)
{
	return MTL4BufferRangeMake(buffer.gpuAddress + offset, buffer.length - offset);
}

static std::vector<float> LoadRays(const char* path)
{
	std::vector<float> rays;
	FILE* file = fopen(path, "r");
	if (!file)
	{
		fprintf(stderr, "can't read %s\n", path);
		exit(1);
	}
	char line[1024];
	while (fgets(line, sizeof(line), file))
	{
		if (char* comment = strchr(line, '#'))
			*comment = 0;
		float v[7];
		if (sscanf(line, "%f %f %f %f %f %f %f", &v[0], &v[1], &v[2], &v[3], &v[4], &v[5], &v[6]) != 7)
			continue;
		float ray[8] = {v[0], v[1], v[2], v[6], v[3], v[4], v[5], 0.0f};
		rays.insert(rays.end(), ray, ray + 8);
	}
	fclose(file);
	return rays;
}

// present: a window behind the others presenting a cleared frame every refresh (see the top)
static int RunPresent(double seconds)
{
	[NSApplication sharedApplication];
	[NSApp setActivationPolicy:NSApplicationActivationPolicyAccessory]; // no dock icon, never takes the focus
	[NSApp finishLaunching];
	NSRect contentRect = NSMakeRect(0, 0, 1280, 720);
	NSWindow* window = [[NSWindow alloc] initWithContentRect:contentRect
												   styleMask:NSWindowStyleMaskTitled
													 backing:NSBackingStoreBuffered
													   defer:NO];
	window.title = @"rtrepro present";
	id<MTLDevice> device = MTLCreateSystemDefaultDevice();
	CAMetalLayer* layer = [CAMetalLayer layer];
	layer.device = device;
	layer.pixelFormat = MTLPixelFormatBGRA8Unorm;
	layer.displaySyncEnabled = YES;
	layer.maximumDrawableCount = 3;
	layer.contentsScale = window.backingScaleFactor;
	layer.drawableSize = CGSizeMake(contentRect.size.width * window.backingScaleFactor, contentRect.size.height * window.backingScaleFactor);
	window.contentView.wantsLayer = YES;
	window.contentView.layer = layer;
	[window orderBack:nil];

	id<MTL4CommandQueue> queue = [device newMTL4CommandQueue];
	[queue addResidencySet:layer.residencySet];
	std::vector<id<MTL4CommandAllocator>> allocators;
	for (uint32_t frameIt = 0; frameIt < kFramesInFlight; frameIt++)
		allocators.push_back([device newCommandAllocator]);
	id<MTLSharedEvent> event = [device newSharedEvent];
	uint64_t eventValue = 0;
	printf("present: a window of %.0fx%.0f pixels, a frame every refresh\n", layer.drawableSize.width, layer.drawableSize.height);
	fflush(stdout);

	NSDate* start = [NSDate date];
	double lastPrint = 0.0;
	uint64_t frame = 0;
	uint64_t framesSincePrint = 0;
	while ([[NSDate date] timeIntervalSinceDate:start] < seconds && gErrors.load() == 0)
	{
		@autoreleasepool
		{
			while (NSEvent* windowEvent = [NSApp nextEventMatchingMask:NSEventMaskAny untilDate:nil inMode:NSDefaultRunLoopMode dequeue:YES])
				[NSApp sendEvent:windowEvent];
			uint32_t slot = static_cast<uint32_t>(frame % kFramesInFlight);
			if (frame >= kFramesInFlight)
			{
				if (![event waitUntilSignaledValue:eventValue - (kFramesInFlight - 1) timeoutMS:10000])
				{
					fprintf(stderr, "frame %llu not done within 10 s\n", frame - kFramesInFlight);
					return 3;
				}
				[allocators[slot] reset];
			}
			id<CAMetalDrawable> drawable = [layer nextDrawable];
			if (!drawable)
				continue;
			id<MTL4CommandBuffer> commandBuffer = [device newCommandBuffer];
			[commandBuffer beginCommandBufferWithAllocator:allocators[slot]];
			MTL4RenderPassDescriptor* pass = [MTL4RenderPassDescriptor new];
			pass.colorAttachments[0].texture = drawable.texture;
			pass.colorAttachments[0].loadAction = MTLLoadActionClear;
			pass.colorAttachments[0].storeAction = MTLStoreActionStore;
			pass.colorAttachments[0].clearColor = MTLClearColorMake(0.5 + 0.5 * sin(frame * 0.05), 0.3, 0.6, 1.0);
			id<MTL4RenderCommandEncoder> encoder = [commandBuffer renderCommandEncoderWithDescriptor:pass];
			[encoder endEncoding];
			[commandBuffer endCommandBuffer];
			MTL4CommitOptions* options = [MTL4CommitOptions new];
			[options addFeedbackHandler:^(id<MTL4CommitFeedback> feedback) {
				if (feedback.error)
				{
					gErrors++;
					fprintf(stderr, "commit error %ld: %s\n", (long)feedback.error.code, feedback.error.localizedDescription.UTF8String);
				}
			}];
			[queue waitForDrawable:drawable];
			id<MTL4CommandBuffer> buffers[] = {commandBuffer};
			[queue commit:buffers count:1 options:options];
			[queue signalDrawable:drawable];
			[drawable present];
			[queue signalEvent:event value:++eventValue];
			frame++;
			framesSincePrint++;
			double now = [[NSDate date] timeIntervalSinceDate:start];
			if (now - lastPrint >= 1.0)
			{
				printf("%.0f s: %llu frames (%.1f a second)\n", now, frame, framesSincePrint / (now - lastPrint));
				fflush(stdout);
				lastPrint = now;
				framesSincePrint = 0;
			}
		}
	}
	[event waitUntilSignaledValue:eventValue timeoutMS:10000];
	printf("done: %llu frames, %d errors\n", frame, gErrors.load());
	return gErrors.load() == 0 ? 0 : 4;
}

// frame: what a speedo frame on KosmicKrisp does around its ray queries (see the top)
// the frame mode's first kernels, as they first restarted the gpu (flag 0)
static const char* kFrameSource0 = R"(
#include <metal_stdlib>
#include <metal_raytracing>
using namespace metal;
using namespace raytracing;

struct FrameParams
{
	uint trace;
	uint frame;
	uint width;
	uint height;
};

struct FullScreen
{
	float4 position [[position]];
	float2 uv;
};

vertex FullScreen frame_vertex(uint vid [[vertex_id]])
{
	float2 uv = float2((vid << 1) & 2, vid & 2);
	FullScreen out;
	out.position = float4(uv * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.5, 1.0);
	out.uv = uv;
	return out;
}

struct PrepassOut
{
	float4 color [[color(0)]];
	float depth [[depth(any)]];
};

fragment PrepassOut frame_prepass(FullScreen in [[stage_in]], constant FrameParams& params [[buffer(0)]])
{
	PrepassOut out;
	float wave = 0.5 + 0.25 * sin(in.uv.x * 20.0 + float(params.frame) * 0.1) * cos(in.uv.y * 13.0);
	out.color = float4(in.uv, wave, 1.0);
	out.depth = wave;
	return out;
}

fragment float4 frame_ui(FullScreen in [[stage_in]], texture2d<float> shaded [[texture(0)]])
{
	constexpr sampler linear(filter::linear, address::clamp_to_edge);
	return shaded.sample(linear, in.uv);
}

template <bool RT>
float4 Shade(uint2 pixel, constant FrameParams& params, texture2d<float, access::read> depth, texture2d<float, access::read> color,
	instance_acceleration_structure structure)
{
	float d = depth.read(pixel).r;
	float4 c = color.read(pixel);
	float lit = 1.0;
	if (RT)
	{
		intersection_query<instancing, triangle_data> query;
		if (params.trace != 0)
		{
			float2 uv = (float2(pixel) + 0.5) / float2(params.width, params.height);
			float3 origin = float3(uv * 4.0 - 2.0, -2.0 + d);
			intersection_params intersectionParams;
			intersectionParams.set_geometry_cull_mode(geometry_cull_mode::bounding_box);
			query.reset(ray(origin, normalize(float3(0.2, 0.3, 1.0)), 0.0, 8.0), structure, 0xFF, intersectionParams);
			while (query.next())
			{
			}
			if (query.get_committed_intersection_type() == intersection_type::triangle)
				lit = 0.25;
		}
	}
	return float4(c.rgb * lit * (0.5 + 0.5 * d), 1.0);
}

kernel void frame_shade_rt(
	constant FrameParams& params [[buffer(0)]],
	instance_acceleration_structure structure [[buffer(1)]],
	texture2d<float, access::read> depth [[texture(0)]],
	texture2d<float, access::read> color [[texture(1)]],
	texture2d<float, access::write> shaded [[texture(2)]],
	uint2 pixel [[thread_position_in_grid]])
{
	if (pixel.x >= params.width || pixel.y >= params.height)
		return;
	shaded.write(Shade<true>(pixel, params, depth, color, structure), pixel);
}

kernel void frame_shade(
	constant FrameParams& params [[buffer(0)]],
	texture2d<float, access::read> depth [[texture(0)]],
	texture2d<float, access::read> color [[texture(1)]],
	texture2d<float, access::write> shaded [[texture(2)]],
	uint2 pixel [[thread_position_in_grid]])
{
	if (pixel.x >= params.width || pixel.y >= params.height)
		return;
	shaded.write(Shade<false>(pixel, params, depth, color, instance_acceleration_structure()), pixel);
}
)";

static const char* kFrameSource = R"(
#include <metal_stdlib>
#include <metal_raytracing>
using namespace metal;
using namespace raytracing;

struct FrameParams
{
	uint trace;
	uint frame;
	uint width;
	uint height;
	uint reads;
};

struct FullScreen
{
	float4 position [[position]];
	float2 uv;
};

vertex FullScreen frame_vertex(uint vid [[vertex_id]])
{
	float2 uv = float2((vid << 1) & 2, vid & 2);
	FullScreen out;
	out.position = float4(uv * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.5, 1.0);
	out.uv = uv;
	return out;
}

struct PrepassOut
{
	float4 color [[color(0)]];
	float depth [[depth(any)]];
};

fragment PrepassOut frame_prepass(FullScreen in [[stage_in]], constant FrameParams& params [[buffer(0)]])
{
	PrepassOut out;
	float wave = 0.5 + 0.25 * sin(in.uv.x * 20.0 + float(params.frame) * 0.1) * cos(in.uv.y * 13.0);
	out.color = float4(in.uv, wave, 1.0);
	out.depth = wave;
	return out;
}

fragment float4 frame_ui(FullScreen in [[stage_in]], texture2d<float> shaded [[texture(0)]])
{
	constexpr sampler linear(filter::linear, address::clamp_to_edge);
	return shaded.sample(linear, in.uv);
}

// a pixel's shading from the pre-pass's depth and color: with RT, a ray toward the light first (when trace is set)
// (RT 1: an intersection query, as KosmicKrisp lowers a Vulkan ray query; 2: an intersector's one call)
template <int RT>
float4 Shade(uint2 pixel, constant FrameParams& params, texture2d<float, access::read> depth, texture2d<float, access::read> color,
	instance_acceleration_structure structure)
{
	float d = depth.read(pixel).r;
	float4 c = color.read(pixel);
	float lit = 1.0;
	if (RT == 2)
	{
		if (params.trace != 0)
		{
			float2 uv = (float2(pixel) + 0.5) / float2(params.width, params.height);
			float3 origin = float3(uv * 4.0 - 2.0, -2.0 + d);
			intersector<instancing, triangle_data> isect;
			isect.assume_geometry_type(geometry_type::triangle);
			isect.force_opacity(forced_opacity::opaque);
			intersection_result<instancing, triangle_data> hit = isect.intersect(ray(origin, normalize(float3(0.2, 0.3, 1.0)), 0.0, 8.0), structure, 0xFF);
			if (hit.type == intersection_type::triangle)
				lit = 0.25;
		}
	}
	if (RT == 1)
	{
		intersection_query<instancing, triangle_data> query;
		if (params.trace != 0)
		{
			float2 uv = (float2(pixel) + 0.5) / float2(params.width, params.height);
			float3 origin = float3(uv * 4.0 - 2.0, -2.0 + d);
			intersection_params intersectionParams;
			intersectionParams.set_geometry_cull_mode(geometry_cull_mode::bounding_box);
			query.reset(ray(origin, normalize(float3(0.2, 0.3, 1.0)), 0.0, 8.0), structure, 0xFF, intersectionParams);
			while (query.next())
			{
			}
			if (query.get_committed_intersection_type() == intersection_type::triangle)
				lit = 0.25;
		}
	}
	return float4(c.rgb * lit * (0.5 + 0.5 * d), 1.0);
}

kernel void frame_shade_rt(
	constant FrameParams& params [[buffer(0)]],
	instance_acceleration_structure structure [[buffer(1)]],
	texture2d<float, access::read> depth [[texture(0)]],
	texture2d<float, access::read> color [[texture(1)]],
	texture2d<float, access::write> shaded [[texture(2)]],
	uint2 pixel [[thread_position_in_grid]])
{
	if (pixel.x >= params.width || pixel.y >= params.height)
		return;
	shaded.write(Shade<1>(pixel, params, depth, color, structure), pixel);
}

kernel void frame_shade_isect(
	constant FrameParams& params [[buffer(0)]],
	instance_acceleration_structure structure [[buffer(1)]],
	texture2d<float, access::read> depth [[texture(0)]],
	texture2d<float, access::read> color [[texture(1)]],
	texture2d<float, access::write> shaded [[texture(2)]],
	uint2 pixel [[thread_position_in_grid]])
{
	if (pixel.x >= params.width || pixel.y >= params.height)
		return;
	shaded.write(Shade<2>(pixel, params, depth, color, structure), pixel);
}

kernel void frame_shade(
	constant FrameParams& params [[buffer(0)]],
	texture2d<float, access::read> depth [[texture(0)]],
	texture2d<float, access::read> color [[texture(1)]],
	texture2d<float, access::write> shaded [[texture(2)]],
	uint2 pixel [[thread_position_in_grid]])
{
	if (pixel.x >= params.width || pixel.y >= params.height)
		return;
	shaded.write(Shade<0>(pixel, params, depth, color, instance_acceleration_structure()), pixel);
}
)";

static int RunFrame(double seconds, const char* flags)
{
	// r: ray tracing compiled in, t: tracing, s: a counter heap resolved after the render passes, p: the pre-pass, u: the
	// pass into the drawable, d: presenting (else into a target of its own, at about the display's rate), x: the
	// kernel's reads of the pre-pass's targets
	auto has = [flags](char flag) { return strchr(flags, flag) != nullptr; };
	bool rayTracing = has('r');
	bool trace = has('t');
	bool resolveAfterRender = has('s');
	bool prepasses = has('p');
	bool uis = has('u');
	bool presents = has('d');
	bool reads = has('x');
	bool splits = has('c');
	[NSApplication sharedApplication];
	[NSApp setActivationPolicy:NSApplicationActivationPolicyAccessory]; // no dock icon, never takes the focus
	[NSApp finishLaunching];
	NSRect contentRect = NSMakeRect(0, 0, 1280, 720);
	NSWindow* window = [[NSWindow alloc] initWithContentRect:contentRect
												   styleMask:NSWindowStyleMaskTitled
													 backing:NSBackingStoreBuffered
													   defer:NO];
	window.title = @"rtrepro frame";
	id<MTLDevice> device = MTLCreateSystemDefaultDevice();
	CAMetalLayer* layer = [CAMetalLayer layer];
	layer.device = device;
	layer.pixelFormat = MTLPixelFormatBGRA8Unorm;
	layer.displaySyncEnabled = YES;
	layer.maximumDrawableCount = 3;
	layer.contentsScale = window.backingScaleFactor;
	layer.drawableSize = CGSizeMake(contentRect.size.width * window.backingScaleFactor, contentRect.size.height * window.backingScaleFactor);
	window.contentView.wantsLayer = YES;
	window.contentView.layer = layer;
	[window orderBack:nil];
	// h: the window off every screen, so that WindowServer has nothing of it to composite
	if (has('h'))
		[window setFrameOrigin:NSMakePoint(-100000, -100000)];

	NSError* error = nil;
	id<MTLLibrary> library = [device newLibraryWithSource:@(has('0') ? kFrameSource0 : kFrameSource) options:nil error:&error];
	if (!library)
	{
		fprintf(stderr, "library: %s\n", error.localizedDescription.UTF8String);
		return 1;
	}
	id<MTLComputePipelineState> shade =
		[device newComputePipelineStateWithFunction:[library newFunctionWithName:!rayTracing ? @"frame_shade" : has('i') ? @"frame_shade_isect" : @"frame_shade_rt"] error:&error];
	MTLRenderPipelineDescriptor* prepassDesc = [MTLRenderPipelineDescriptor new];
	prepassDesc.vertexFunction = [library newFunctionWithName:@"frame_vertex"];
	prepassDesc.fragmentFunction = [library newFunctionWithName:@"frame_prepass"];
	prepassDesc.colorAttachments[0].pixelFormat = MTLPixelFormatRGBA8Unorm;
	prepassDesc.depthAttachmentPixelFormat = MTLPixelFormatDepth32Float;
	id<MTLRenderPipelineState> prepass = [device newRenderPipelineStateWithDescriptor:prepassDesc error:&error];
	MTLRenderPipelineDescriptor* uiDesc = [MTLRenderPipelineDescriptor new];
	uiDesc.vertexFunction = [library newFunctionWithName:@"frame_vertex"];
	uiDesc.fragmentFunction = [library newFunctionWithName:@"frame_ui"];
	uiDesc.colorAttachments[0].pixelFormat = MTLPixelFormatBGRA8Unorm;
	id<MTLRenderPipelineState> ui = [device newRenderPipelineStateWithDescriptor:uiDesc error:&error];
	if (!shade || !prepass || !ui)
	{
		fprintf(stderr, "pipeline: %s\n", error.localizedDescription.UTF8String);
		return 1;
	}
	MTLDepthStencilDescriptor* depthDesc = [MTLDepthStencilDescriptor new];
	depthDesc.depthCompareFunction = MTLCompareFunctionAlways;
	depthDesc.depthWriteEnabled = YES;
	id<MTLDepthStencilState> depthState = [device newDepthStencilStateWithDescriptor:depthDesc];

	// the pre-pass's targets and the shading's, at the window's size in points (speedo's 1280x720 render resolution)
	const uint32_t width = 1280;
	const uint32_t height = 720;
	auto texture = [&](MTLPixelFormat format, MTLTextureUsage usage)
	{
		MTLTextureDescriptor* desc = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:format width:width height:height mipmapped:NO];
		desc.storageMode = MTLStorageModePrivate;
		desc.usage = usage;
		return [device newTextureWithDescriptor:desc];
	};
	id<MTLTexture> depth = texture(MTLPixelFormatDepth32Float, MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead);
	id<MTLTexture> color = texture(MTLPixelFormatRGBA8Unorm, MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead);
	id<MTLTexture> shaded = texture(MTLPixelFormatRGBA16Float, MTLTextureUsageShaderRead | MTLTextureUsageShaderWrite);
	MTLTextureDescriptor* offscreenDesc = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
																							  width:static_cast<NSUInteger>(layer.drawableSize.width)
																							 height:static_cast<NSUInteger>(layer.drawableSize.height)
																						  mipmapped:NO];
	offscreenDesc.storageMode = MTLStorageModePrivate;
	offscreenDesc.usage = MTLTextureUsageRenderTarget;
	id<MTLTexture> offscreen = [device newTextureWithDescriptor:offscreenDesc];
	id<MTLBuffer> params = [device newBufferWithLength:256 * kFramesInFlight options:MTLResourceStorageModeShared];

	id<MTL4CommandQueue> queue = [device newMTL4CommandQueue];
	[queue addResidencySet:layer.residencySet];
	MTLResidencySetDescriptor* residencyDesc = [MTLResidencySetDescriptor new];
	id<MTLResidencySet> residency = [device newResidencySetWithDescriptor:residencyDesc error:&error];
	[queue addResidencySet:residency];
	for (id<MTLAllocation> allocation : std::vector<id<MTLAllocation>>{depth, color, shaded, offscreen, params})
		[residency addAllocation:allocation];
	MTL4ArgumentTableDescriptor* tableDesc = [MTL4ArgumentTableDescriptor new];
	tableDesc.maxBufferBindCount = 2;
	tableDesc.maxTextureBindCount = 3;
	std::vector<id<MTL4ArgumentTable>> computeTables;
	std::vector<id<MTL4ArgumentTable>> renderTables;
	std::vector<id<MTL4CommandAllocator>> allocators;
	for (uint32_t frameIt = 0; frameIt < kFramesInFlight; frameIt++)
	{
		computeTables.push_back([device newArgumentTableWithDescriptor:tableDesc error:&error]);
		renderTables.push_back([device newArgumentTableWithDescriptor:tableDesc error:&error]);
		allocators.push_back([device newCommandAllocator]);
	}
	id<MTLSharedEvent> event = [device newSharedEvent];
	uint64_t eventValue = 0;
	auto commitOptions = []
	{
		MTL4CommitOptions* options = [MTL4CommitOptions new];
		[options addFeedbackHandler:^(id<MTL4CommitFeedback> feedback) {
			if (feedback.error)
			{
				gErrors++;
				fprintf(stderr, "commit error %ld: %s\n", (long)feedback.error.code, feedback.error.localizedDescription.UTF8String);
			}
		}];
		return options;
	};

	// a unit cube's primitive structure and an instance structure of it, built once
	id<MTLAccelerationStructure> instanceStructure = nil;
	if (rayTracing)
	{
		const float vertices[8][3] = {{-0.5f, -0.5f, -0.5f}, {0.5f, -0.5f, -0.5f}, {0.5f, 0.5f, -0.5f}, {-0.5f, 0.5f, -0.5f},
									  {-0.5f, -0.5f, 0.5f},  {0.5f, -0.5f, 0.5f},  {0.5f, 0.5f, 0.5f},  {-0.5f, 0.5f, 0.5f}};
		const uint32_t indices[36] = {0, 2, 1, 0, 3, 2, 4, 5, 6, 4, 6, 7, 0, 1, 5, 0, 5, 4, 3, 6, 2, 3, 7, 6, 0, 4, 7, 0, 7, 3, 1, 2, 6, 1, 6, 5};
		id<MTLBuffer> vertexBuffer = [device newBufferWithBytes:vertices length:sizeof(vertices) options:MTLResourceStorageModeShared];
		id<MTLBuffer> indexBuffer = [device newBufferWithBytes:indices length:sizeof(indices) options:MTLResourceStorageModeShared];
		MTL4AccelerationStructureTriangleGeometryDescriptor* geometry = [MTL4AccelerationStructureTriangleGeometryDescriptor new];
		geometry.vertexBuffer = Range(vertexBuffer);
		geometry.vertexFormat = MTLAttributeFormatFloat3;
		geometry.vertexStride = sizeof(float) * 3;
		geometry.indexBuffer = Range(indexBuffer);
		geometry.indexType = MTLIndexTypeUInt32;
		geometry.triangleCount = 12;
		geometry.opaque = YES;
		MTL4PrimitiveAccelerationStructureDescriptor* primitiveDesc = [MTL4PrimitiveAccelerationStructureDescriptor new];
		primitiveDesc.geometryDescriptors = @[geometry];
		MTLAccelerationStructureSizes primitiveSizes = [device accelerationStructureSizesWithDescriptor:primitiveDesc];
		id<MTLAccelerationStructure> primitive = [device newAccelerationStructureWithSize:primitiveSizes.accelerationStructureSize];
		id<MTLBuffer> instances = [device newBufferWithLength:sizeof(MTLIndirectAccelerationStructureInstanceDescriptor) options:MTLResourceStorageModeShared];
		auto* instance = static_cast<MTLIndirectAccelerationStructureInstanceDescriptor*>(instances.contents);
		memset(instance, 0, sizeof(*instance));
		instance->transformationMatrix.columns[0] = MTLPackedFloat3Make(1.0f, 0.0f, 0.0f);
		instance->transformationMatrix.columns[1] = MTLPackedFloat3Make(0.0f, 1.0f, 0.0f);
		instance->transformationMatrix.columns[2] = MTLPackedFloat3Make(0.0f, 0.0f, 1.0f);
		instance->options = MTLAccelerationStructureInstanceOptionOpaque;
		instance->mask = 0xFF;
		instance->accelerationStructureID = primitive.gpuResourceID;
		MTL4InstanceAccelerationStructureDescriptor* instanceDesc = [MTL4InstanceAccelerationStructureDescriptor new];
		instanceDesc.instanceDescriptorBuffer = Range(instances);
		instanceDesc.instanceDescriptorStride = sizeof(MTLIndirectAccelerationStructureInstanceDescriptor);
		instanceDesc.instanceCount = 1;
		instanceDesc.instanceDescriptorType = MTLAccelerationStructureInstanceDescriptorTypeIndirect;
		MTLAccelerationStructureSizes instanceSizes = [device accelerationStructureSizesWithDescriptor:instanceDesc];
		instanceStructure = [device newAccelerationStructureWithSize:instanceSizes.accelerationStructureSize];
		id<MTLBuffer> scratch =
			[device newBufferWithLength:std::max(primitiveSizes.buildScratchBufferSize, instanceSizes.buildScratchBufferSize) options:MTLResourceStorageModePrivate];
		for (id<MTLAllocation> allocation : std::vector<id<MTLAllocation>>{vertexBuffer, indexBuffer, instances, scratch, primitive, instanceStructure})
			[residency addAllocation:allocation];
		[residency commit];
		[residency requestResidency];
		id<MTL4CommandBuffer> commandBuffer = [device newCommandBuffer];
		[commandBuffer beginCommandBufferWithAllocator:allocators[0]];
		id<MTL4ComputeCommandEncoder> encoder = [commandBuffer computeCommandEncoder];
		[encoder buildAccelerationStructure:primitive descriptor:primitiveDesc scratchBuffer:Range(scratch)];
		[encoder barrierAfterEncoderStages:MTLStageAccelerationStructure
					   beforeEncoderStages:MTLStageAccelerationStructure
						 visibilityOptions:MTL4VisibilityOptionNone];
		[encoder buildAccelerationStructure:instanceStructure descriptor:instanceDesc scratchBuffer:Range(scratch)];
		[encoder endEncoding];
		[commandBuffer endCommandBuffer];
		id<MTL4CommandBuffer> buffers[] = {commandBuffer};
		[queue commit:buffers count:1 options:commitOptions()];
		[queue signalEvent:event value:++eventValue];
		if (![event waitUntilSignaledValue:eventValue timeoutMS:10000])
		{
			fprintf(stderr, "the structures' builds didn't finish\n");
			return 1;
		}
		[allocators[0] reset];
	}
	else
	{
		[residency commit];
		[residency requestResidency];
	}
	// what KosmicKrisp's profile builds resolve after the encoders that hold timestamps
	MTL4CounterHeapDescriptor* heapDesc = [MTL4CounterHeapDescriptor new];
	heapDesc.type = MTL4CounterHeapTypeTimestamp;
	heapDesc.count = 1;
	id<MTL4CounterHeap> counterHeap = [device newCounterHeapWithDescriptor:heapDesc error:&error];
	id<MTLBuffer> counterBuffer = [device newBufferWithLength:4096 options:MTLResourceStorageModeShared];
	[residency addAllocation:counterBuffer];
	[residency commit];

	printf("frame: %ux%u, a pre-pass, a %skernel%s, then the drawable, every refresh%s\n", width, height,
		   rayTracing ? "ray tracing " : "", rayTracing ? (trace ? " that traces" : " that doesn't trace") : "",
		   resolveAfterRender ? ", a counter heap resolved after each render pass" : "");
	fflush(stdout);

	// b: burn's load in this process, on a queue of its own fed from a GCD queue (rather than in another process)
	std::atomic<bool> burnStop{false};
	dispatch_semaphore_t burnDone = dispatch_semaphore_create(0);
	if (has('b'))
	{
		id<MTLLibrary> burnLibrary = [device newLibraryWithSource:@(kSource) options:nil error:&error];
		id<MTLComputePipelineState> burn = [device newComputePipelineStateWithFunction:[burnLibrary newFunctionWithName:@"burn"] error:&error];
		id<MTL4CommandQueue> burnQueue = [device newMTL4CommandQueue];
		id<MTLResidencySet> burnResidency = [device newResidencySetWithDescriptor:[MTLResidencySetDescriptor new] error:&error];
		[burnQueue addResidencySet:burnResidency];
		id<MTLBuffer> burnOut = [device newBufferWithLength:4096 options:MTLResourceStorageModeShared];
		id<MTLBuffer> burnParams = [device newBufferWithLength:256 options:MTLResourceStorageModeShared];
		*static_cast<uint32_t*>(burnParams.contents) = 2000;
		[burnResidency addAllocation:burnOut];
		[burnResidency addAllocation:burnParams];
		[burnResidency commit];
		[burnResidency requestResidency];
		MTL4ArgumentTableDescriptor* burnTableDesc = [MTL4ArgumentTableDescriptor new];
		burnTableDesc.maxBufferBindCount = 2;
		id<MTL4ArgumentTable> burnTable = [device newArgumentTableWithDescriptor:burnTableDesc error:&error];
		id<MTL4CommandAllocator> burnAllocator = [device newCommandAllocator];
		id<MTLSharedEvent> burnEvent = [device newSharedEvent];
		std::atomic<bool>* stop = &burnStop;
		dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
			uint64_t value = 0;
			while (!stop->load())
			{
				@autoreleasepool
				{
					id<MTL4CommandBuffer> commandBuffer = [device newCommandBuffer];
					[commandBuffer beginCommandBufferWithAllocator:burnAllocator];
					id<MTL4ComputeCommandEncoder> encoder = [commandBuffer computeCommandEncoder];
					[burnTable setAddress:burnOut.gpuAddress atIndex:0];
					[burnTable setAddress:burnParams.gpuAddress atIndex:1];
					[encoder setArgumentTable:burnTable];
					[encoder setComputePipelineState:burn];
					[encoder dispatchThreads:MTLSizeMake(1u << 20, 1, 1) threadsPerThreadgroup:MTLSizeMake(64, 1, 1)];
					[encoder endEncoding];
					[commandBuffer endCommandBuffer];
					id<MTL4CommandBuffer> buffers[] = {commandBuffer};
					[burnQueue commit:buffers count:1 options:commitOptions()];
					[burnQueue signalEvent:burnEvent value:++value];
					if (![burnEvent waitUntilSignaledValue:value timeoutMS:10000])
					{
						fprintf(stderr, "burn: a dispatch not done within 10 s\n");
						gErrors++;
						break;
					}
					[burnAllocator reset];
				}
			}
			dispatch_semaphore_signal(burnDone);
		});
		printf("with burn's load in this process, on its own queue\n");
		fflush(stdout);
	}

	// as KosmicKrisp ends every encoder: a producer barrier on everything before what follows
	auto producerBarrier = [](id<MTL4CommandEncoder> encoder)
	{
		[encoder barrierAfterStages:MTLStageAll beforeQueueStages:MTLStageAll visibilityOptions:MTL4VisibilityOptionResourceAlias];
	};
	auto resolve = [&](id<MTL4CommandBuffer> commandBuffer)
	{
		if (resolveAfterRender)
			[commandBuffer resolveCounterHeap:counterHeap
									withRange:NSMakeRange(0, 1)
								   intoBuffer:MTL4BufferRangeMake(counterBuffer.gpuAddress, sizeof(MTL4TimestampHeapEntry))
									waitFence:nil
								  updateFence:nil];
	};

	NSDate* start = [NSDate date];
	double lastPrint = 0.0;
	uint64_t frame = 0;
	uint64_t framesSincePrint = 0;
	while ([[NSDate date] timeIntervalSinceDate:start] < seconds && gErrors.load() == 0)
	{
		@autoreleasepool
		{
			while (NSEvent* windowEvent = [NSApp nextEventMatchingMask:NSEventMaskAny untilDate:nil inMode:NSDefaultRunLoopMode dequeue:YES])
				[NSApp sendEvent:windowEvent];
			uint32_t slot = static_cast<uint32_t>(frame % kFramesInFlight);
			if (frame >= kFramesInFlight)
			{
				if (![event waitUntilSignaledValue:eventValue - (kFramesInFlight - 1) timeoutMS:10000])
				{
					fprintf(stderr, "frame %llu not done within 10 s\n", frame - kFramesInFlight);
					return 3;
				}
				[allocators[slot] reset];
			}
			id<CAMetalDrawable> drawable = presents ? [layer nextDrawable] : nil;
			if (presents && !drawable)
				continue;
			uint32_t frameParams[8] = {trace ? 1u : 0u, static_cast<uint32_t>(frame), width, height, reads ? 1u : 0u, 0, 0, 0};
			memcpy(static_cast<uint8_t*>(params.contents) + 256 * slot, frameParams, sizeof(frameParams));
			id<MTL4CommandBuffer> commandBuffer = [device newCommandBuffer];
			[commandBuffer beginCommandBufferWithAllocator:allocators[slot]];
			id<MTL4RenderCommandEncoder> render = nil;
			// c: the compute in a command buffer of its own, between the render passes' (committed together)
			std::vector<id<MTL4CommandBuffer>> frameBuffers;
			auto split = [&]
			{
				if (!splits)
					return;
				[commandBuffer endCommandBuffer];
				frameBuffers.push_back(commandBuffer);
				commandBuffer = [device newCommandBuffer];
				[commandBuffer beginCommandBufferWithAllocator:allocators[slot]];
			};

			// the pre-pass
			if (prepasses)
			{
				MTL4RenderPassDescriptor* prepassPass = [MTL4RenderPassDescriptor new];
				prepassPass.colorAttachments[0].texture = color;
				prepassPass.colorAttachments[0].loadAction = MTLLoadActionClear;
				prepassPass.colorAttachments[0].storeAction = MTLStoreActionStore;
				prepassPass.depthAttachment.texture = depth;
				prepassPass.depthAttachment.loadAction = MTLLoadActionClear;
				prepassPass.depthAttachment.storeAction = MTLStoreActionStore;
				render = [commandBuffer renderCommandEncoderWithDescriptor:prepassPass];
				id<MTL4ArgumentTable> renderTable = renderTables[slot];
				[renderTable setAddress:params.gpuAddress + 256 * slot atIndex:0];
				[render setArgumentTable:renderTable atStages:MTLRenderStageVertex | MTLRenderStageFragment];
				[render setRenderPipelineState:prepass];
				[render setDepthStencilState:depthState];
				[render drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
				producerBarrier(render);
				[render endEncoding];
				resolve(commandBuffer);
			}
			split();

			// the shading, from the pre-pass's targets
			id<MTL4ComputeCommandEncoder> compute = [commandBuffer computeCommandEncoder];
			id<MTL4ArgumentTable> computeTable = computeTables[slot];
			[computeTable setAddress:params.gpuAddress + 256 * slot atIndex:0];
			if (rayTracing)
				[computeTable setResource:instanceStructure.gpuResourceID atBufferIndex:1];
			[computeTable setTexture:depth.gpuResourceID atIndex:0];
			[computeTable setTexture:color.gpuResourceID atIndex:1];
			[computeTable setTexture:shaded.gpuResourceID atIndex:2];
			[compute setArgumentTable:computeTable];
			[compute setComputePipelineState:shade];
			[compute dispatchThreads:MTLSizeMake(width, height, 1) threadsPerThreadgroup:MTLSizeMake(8, 8, 1)];
			producerBarrier(compute);
			[compute endEncoding];
			split();

			// the drawable (or an offscreen target of its size), from the shading
			if (uis)
			{
				MTL4RenderPassDescriptor* uiPass = [MTL4RenderPassDescriptor new];
				uiPass.colorAttachments[0].texture = presents ? drawable.texture : offscreen;
				uiPass.colorAttachments[0].loadAction = MTLLoadActionDontCare;
				uiPass.colorAttachments[0].storeAction = MTLStoreActionStore;
				render = [commandBuffer renderCommandEncoderWithDescriptor:uiPass];
				id<MTL4ArgumentTable> uiTable = renderTables[slot];
				[uiTable setTexture:shaded.gpuResourceID atIndex:0];
				[render setArgumentTable:uiTable atStages:MTLRenderStageVertex | MTLRenderStageFragment];
				[render setRenderPipelineState:ui];
				[render drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
				producerBarrier(render);
				[render endEncoding];
				resolve(commandBuffer);
			}
			[commandBuffer endCommandBuffer];

			if (presents)
				[queue waitForDrawable:drawable];
			frameBuffers.push_back(commandBuffer);
			[queue commit:frameBuffers.data() count:frameBuffers.size() options:commitOptions()];
			if (presents)
			{
				[queue signalDrawable:drawable];
				[drawable present];
			}
			[queue signalEvent:event value:++eventValue];
			if (!presents)
				usleep(8000); // about the display's rate
			frame++;
			framesSincePrint++;
			double now = [[NSDate date] timeIntervalSinceDate:start];
			if (now - lastPrint >= 1.0)
			{
				printf("%.0f s: %llu frames (%.1f a second)\n", now, frame, framesSincePrint / (now - lastPrint));
				fflush(stdout);
				lastPrint = now;
				framesSincePrint = 0;
			}
		}
	}
	[event waitUntilSignaledValue:eventValue timeoutMS:10000];
	if (has('b'))
	{
		burnStop = true;
		dispatch_semaphore_wait(burnDone, dispatch_time(DISPATCH_TIME_NOW, 15 * NSEC_PER_SEC));
	}
	printf("done: %llu frames, %d errors\n", frame, gErrors.load());
	return gErrors.load() == 0 ? 0 : 4;
}

// frame with flag m: the same frame through the classic queue (an MTLCommandQueue, its encoders and automatic hazard
// tracking, presentDrawable), the same kernels
static int RunFrameClassic(double seconds, const char* flags)
{
	auto has = [flags](char flag) { return strchr(flags, flag) != nullptr; };
	bool rayTracing = has('r');
	bool trace = has('t');
	bool prepasses = has('p');
	bool uis = has('u');
	bool presents = has('d');
	[NSApplication sharedApplication];
	[NSApp setActivationPolicy:NSApplicationActivationPolicyAccessory];
	[NSApp finishLaunching];
	NSRect contentRect = NSMakeRect(0, 0, 1280, 720);
	NSWindow* window = [[NSWindow alloc] initWithContentRect:contentRect
												   styleMask:NSWindowStyleMaskTitled
													 backing:NSBackingStoreBuffered
													   defer:NO];
	window.title = @"rtrepro frame (classic)";
	id<MTLDevice> device = MTLCreateSystemDefaultDevice();
	CAMetalLayer* layer = [CAMetalLayer layer];
	layer.device = device;
	layer.pixelFormat = MTLPixelFormatBGRA8Unorm;
	layer.displaySyncEnabled = YES;
	layer.maximumDrawableCount = 3;
	layer.contentsScale = window.backingScaleFactor;
	layer.drawableSize = CGSizeMake(contentRect.size.width * window.backingScaleFactor, contentRect.size.height * window.backingScaleFactor);
	window.contentView.wantsLayer = YES;
	window.contentView.layer = layer;
	[window orderBack:nil];

	NSError* error = nil;
	id<MTLLibrary> library = [device newLibraryWithSource:@(kFrameSource) options:nil error:&error];
	if (!library)
	{
		fprintf(stderr, "library: %s\n", error.localizedDescription.UTF8String);
		return 1;
	}
	id<MTLComputePipelineState> shade =
		[device newComputePipelineStateWithFunction:[library newFunctionWithName:rayTracing ? @"frame_shade_rt" : @"frame_shade"] error:&error];
	MTLRenderPipelineDescriptor* prepassDesc = [MTLRenderPipelineDescriptor new];
	prepassDesc.vertexFunction = [library newFunctionWithName:@"frame_vertex"];
	prepassDesc.fragmentFunction = [library newFunctionWithName:@"frame_prepass"];
	prepassDesc.colorAttachments[0].pixelFormat = MTLPixelFormatRGBA8Unorm;
	prepassDesc.depthAttachmentPixelFormat = MTLPixelFormatDepth32Float;
	id<MTLRenderPipelineState> prepass = [device newRenderPipelineStateWithDescriptor:prepassDesc error:&error];
	MTLRenderPipelineDescriptor* uiDesc = [MTLRenderPipelineDescriptor new];
	uiDesc.vertexFunction = [library newFunctionWithName:@"frame_vertex"];
	uiDesc.fragmentFunction = [library newFunctionWithName:@"frame_ui"];
	uiDesc.colorAttachments[0].pixelFormat = MTLPixelFormatBGRA8Unorm;
	id<MTLRenderPipelineState> ui = [device newRenderPipelineStateWithDescriptor:uiDesc error:&error];
	if (!shade || !prepass || !ui)
	{
		fprintf(stderr, "pipeline: %s\n", error.localizedDescription.UTF8String);
		return 1;
	}
	MTLDepthStencilDescriptor* depthDesc = [MTLDepthStencilDescriptor new];
	depthDesc.depthCompareFunction = MTLCompareFunctionAlways;
	depthDesc.depthWriteEnabled = YES;
	id<MTLDepthStencilState> depthState = [device newDepthStencilStateWithDescriptor:depthDesc];

	const uint32_t width = 1280;
	const uint32_t height = 720;
	auto texture = [&](MTLPixelFormat format, MTLTextureUsage usage, NSUInteger w, NSUInteger h)
	{
		MTLTextureDescriptor* desc = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:format width:w height:h mipmapped:NO];
		desc.storageMode = MTLStorageModePrivate;
		desc.usage = usage;
		return [device newTextureWithDescriptor:desc];
	};
	id<MTLTexture> depth = texture(MTLPixelFormatDepth32Float, MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead, width, height);
	id<MTLTexture> color = texture(MTLPixelFormatRGBA8Unorm, MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead, width, height);
	id<MTLTexture> shaded = texture(MTLPixelFormatRGBA16Float, MTLTextureUsageShaderRead | MTLTextureUsageShaderWrite, width, height);
	id<MTLTexture> offscreen = texture(MTLPixelFormatBGRA8Unorm, MTLTextureUsageRenderTarget, static_cast<NSUInteger>(layer.drawableSize.width),
		static_cast<NSUInteger>(layer.drawableSize.height));
	id<MTLBuffer> params = [device newBufferWithLength:256 * kFramesInFlight options:MTLResourceStorageModeShared];
	id<MTLCommandQueue> queue = [device newCommandQueue];

	// the cube's primitive structure and an instance structure of it (classic instance descriptors)
	id<MTLAccelerationStructure> primitive = nil;
	id<MTLAccelerationStructure> instanceStructure = nil;
	if (rayTracing)
	{
		const float vertices[8][3] = {{-0.5f, -0.5f, -0.5f}, {0.5f, -0.5f, -0.5f}, {0.5f, 0.5f, -0.5f}, {-0.5f, 0.5f, -0.5f},
									  {-0.5f, -0.5f, 0.5f},  {0.5f, -0.5f, 0.5f},  {0.5f, 0.5f, 0.5f},  {-0.5f, 0.5f, 0.5f}};
		const uint32_t indices[36] = {0, 2, 1, 0, 3, 2, 4, 5, 6, 4, 6, 7, 0, 1, 5, 0, 5, 4, 3, 6, 2, 3, 7, 6, 0, 4, 7, 0, 7, 3, 1, 2, 6, 1, 6, 5};
		id<MTLBuffer> vertexBuffer = [device newBufferWithBytes:vertices length:sizeof(vertices) options:MTLResourceStorageModeShared];
		id<MTLBuffer> indexBuffer = [device newBufferWithBytes:indices length:sizeof(indices) options:MTLResourceStorageModeShared];
		MTLAccelerationStructureTriangleGeometryDescriptor* geometry = [MTLAccelerationStructureTriangleGeometryDescriptor descriptor];
		geometry.vertexBuffer = vertexBuffer;
		geometry.vertexFormat = MTLAttributeFormatFloat3;
		geometry.vertexStride = sizeof(float) * 3;
		geometry.indexBuffer = indexBuffer;
		geometry.indexType = MTLIndexTypeUInt32;
		geometry.triangleCount = 12;
		geometry.opaque = YES;
		MTLPrimitiveAccelerationStructureDescriptor* primitiveDesc = [MTLPrimitiveAccelerationStructureDescriptor descriptor];
		primitiveDesc.geometryDescriptors = @[geometry];
		MTLAccelerationStructureSizes primitiveSizes = [device accelerationStructureSizesWithDescriptor:primitiveDesc];
		primitive = [device newAccelerationStructureWithSize:primitiveSizes.accelerationStructureSize];
		id<MTLBuffer> instances = [device newBufferWithLength:sizeof(MTLAccelerationStructureInstanceDescriptor) options:MTLResourceStorageModeShared];
		auto* instance = static_cast<MTLAccelerationStructureInstanceDescriptor*>(instances.contents);
		memset(instance, 0, sizeof(*instance));
		instance->transformationMatrix.columns[0] = MTLPackedFloat3Make(1.0f, 0.0f, 0.0f);
		instance->transformationMatrix.columns[1] = MTLPackedFloat3Make(0.0f, 1.0f, 0.0f);
		instance->transformationMatrix.columns[2] = MTLPackedFloat3Make(0.0f, 0.0f, 1.0f);
		instance->options = MTLAccelerationStructureInstanceOptionOpaque;
		instance->mask = 0xFF;
		instance->accelerationStructureIndex = 0;
		MTLInstanceAccelerationStructureDescriptor* instanceDesc = [MTLInstanceAccelerationStructureDescriptor descriptor];
		instanceDesc.instancedAccelerationStructures = @[primitive];
		instanceDesc.instanceDescriptorBuffer = instances;
		instanceDesc.instanceCount = 1;
		MTLAccelerationStructureSizes instanceSizes = [device accelerationStructureSizesWithDescriptor:instanceDesc];
		instanceStructure = [device newAccelerationStructureWithSize:instanceSizes.accelerationStructureSize];
		id<MTLBuffer> scratch =
			[device newBufferWithLength:std::max(primitiveSizes.buildScratchBufferSize, instanceSizes.buildScratchBufferSize) options:MTLResourceStorageModePrivate];
		id<MTLCommandBuffer> commandBuffer = [queue commandBuffer];
		id<MTLAccelerationStructureCommandEncoder> encoder = [commandBuffer accelerationStructureCommandEncoder];
		[encoder buildAccelerationStructure:primitive descriptor:primitiveDesc scratchBuffer:scratch scratchBufferOffset:0];
		[encoder endEncoding];
		[commandBuffer commit];
		[commandBuffer waitUntilCompleted];
		commandBuffer = [queue commandBuffer];
		encoder = [commandBuffer accelerationStructureCommandEncoder];
		[encoder buildAccelerationStructure:instanceStructure descriptor:instanceDesc scratchBuffer:scratch scratchBufferOffset:0];
		[encoder endEncoding];
		[commandBuffer commit];
		[commandBuffer waitUntilCompleted];
		if (commandBuffer.error)
		{
			fprintf(stderr, "the structures' builds failed: %s\n", commandBuffer.error.localizedDescription.UTF8String);
			return 1;
		}
	}
	printf("frame (classic queue): %ux%u, %s a %skernel%s%s\n", width, height, prepasses ? "a pre-pass," : "", rayTracing ? "ray tracing " : "",
		   rayTracing ? (trace ? " that traces" : " that doesn't trace") : "", presents ? ", presented" : ", offscreen");
	fflush(stdout);

	// made at 0 and signaled up, so that releasing it with frames still in flight (after a gpu hang) is no misuse
	dispatch_semaphore_t inFlight = dispatch_semaphore_create(0);
	for (uint32_t frameIt = 0; frameIt < kFramesInFlight; frameIt++)
		dispatch_semaphore_signal(inFlight);
	NSDate* start = [NSDate date];
	double lastPrint = 0.0;
	uint64_t frame = 0;
	uint64_t framesSincePrint = 0;
	while ([[NSDate date] timeIntervalSinceDate:start] < seconds && gErrors.load() == 0)
	{
		@autoreleasepool
		{
			while (NSEvent* windowEvent = [NSApp nextEventMatchingMask:NSEventMaskAny untilDate:nil inMode:NSDefaultRunLoopMode dequeue:YES])
				[NSApp sendEvent:windowEvent];
			if (dispatch_semaphore_wait(inFlight, dispatch_time(DISPATCH_TIME_NOW, 10 * NSEC_PER_SEC)) != 0)
			{
				fprintf(stderr, "frame %llu not done within 10 s\n", frame - kFramesInFlight);
				return 3;
			}
			uint32_t slot = static_cast<uint32_t>(frame % kFramesInFlight);
			id<CAMetalDrawable> drawable = presents ? [layer nextDrawable] : nil;
			if (presents && !drawable)
			{
				dispatch_semaphore_signal(inFlight);
				continue;
			}
			uint32_t frameParams[8] = {trace ? 1u : 0u, static_cast<uint32_t>(frame), width, height, 1u, 0, 0, 0};
			memcpy(static_cast<uint8_t*>(params.contents) + 256 * slot, frameParams, sizeof(frameParams));
			id<MTLCommandBuffer> commandBuffer = [queue commandBuffer];
			if (prepasses)
			{
				MTLRenderPassDescriptor* pass = [MTLRenderPassDescriptor renderPassDescriptor];
				pass.colorAttachments[0].texture = color;
				pass.colorAttachments[0].loadAction = MTLLoadActionClear;
				pass.colorAttachments[0].storeAction = MTLStoreActionStore;
				pass.depthAttachment.texture = depth;
				pass.depthAttachment.loadAction = MTLLoadActionClear;
				pass.depthAttachment.storeAction = MTLStoreActionStore;
				id<MTLRenderCommandEncoder> render = [commandBuffer renderCommandEncoderWithDescriptor:pass];
				[render setRenderPipelineState:prepass];
				[render setDepthStencilState:depthState];
				[render setFragmentBuffer:params offset:256 * slot atIndex:0];
				[render drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
				[render endEncoding];
			}
			id<MTLComputeCommandEncoder> compute = [commandBuffer computeCommandEncoder];
			[compute setComputePipelineState:shade];
			[compute setBuffer:params offset:256 * slot atIndex:0];
			if (rayTracing)
			{
				[compute setAccelerationStructure:instanceStructure atBufferIndex:1];
				[compute useResource:primitive usage:MTLResourceUsageRead];
			}
			[compute setTexture:depth atIndex:0];
			[compute setTexture:color atIndex:1];
			[compute setTexture:shaded atIndex:2];
			[compute dispatchThreads:MTLSizeMake(width, height, 1) threadsPerThreadgroup:MTLSizeMake(8, 8, 1)];
			[compute endEncoding];
			if (uis)
			{
				MTLRenderPassDescriptor* pass = [MTLRenderPassDescriptor renderPassDescriptor];
				pass.colorAttachments[0].texture = presents ? drawable.texture : offscreen;
				pass.colorAttachments[0].loadAction = MTLLoadActionDontCare;
				pass.colorAttachments[0].storeAction = MTLStoreActionStore;
				id<MTLRenderCommandEncoder> render = [commandBuffer renderCommandEncoderWithDescriptor:pass];
				[render setRenderPipelineState:ui];
				[render setFragmentTexture:shaded atIndex:0];
				[render drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
				[render endEncoding];
			}
			if (presents)
				[commandBuffer presentDrawable:drawable];
			[commandBuffer addCompletedHandler:^(id<MTLCommandBuffer> buffer) {
				if (buffer.error)
				{
					gErrors++;
					fprintf(stderr, "command buffer error %ld: %s\n", (long)buffer.error.code, buffer.error.localizedDescription.UTF8String);
				}
				dispatch_semaphore_signal(inFlight);
			}];
			[commandBuffer commit];
			if (!presents)
				usleep(8000);
			frame++;
			framesSincePrint++;
			double now = [[NSDate date] timeIntervalSinceDate:start];
			if (now - lastPrint >= 1.0)
			{
				printf("%.0f s: %llu frames (%.1f a second)\n", now, frame, framesSincePrint / (now - lastPrint));
				fflush(stdout);
				lastPrint = now;
				framesSincePrint = 0;
			}
		}
	}
	for (uint32_t frameIt = 0; frameIt < kFramesInFlight; frameIt++)
		dispatch_semaphore_wait(inFlight, dispatch_time(DISPATCH_TIME_NOW, 10 * NSEC_PER_SEC));
	printf("done: %llu frames, %d errors\n", frame, gErrors.load());
	return gErrors.load() == 0 ? 0 : 4;
}

// compile: pipelines of unique source made and dispatched one after another (see the top)
static int RunCompile(double seconds, bool dispatch, bool deviceCompiler, bool classicQueue)
{
	id<MTLCommandBuffer> classicLast = nil;
	id<MTLDevice> device = MTLCreateSystemDefaultDevice();
	NSError* error = nil;
	// the Metal 4 objects only where they are used: with the classic compiler and queue, none
	id<MTL4Compiler> compiler = nil;
	if (!deviceCompiler)
	{
		compiler = [device newCompilerWithDescriptor:[MTL4CompilerDescriptor new] error:&error];
		if (!compiler)
		{
			fprintf(stderr, "compiler: %s\n", error.localizedDescription.UTF8String);
			return 1;
		}
	}
	id<MTLBuffer> out = [device newBufferWithLength:4096 options:MTLResourceStorageModeShared];
	id<MTL4CommandQueue> queue = nil;
	id<MTL4ArgumentTable> table = nil;
	id<MTL4CommandAllocator> allocator = nil;
	id<MTLSharedEvent> event = nil;
	if (!classicQueue)
	{
		queue = [device newMTL4CommandQueue];
		MTLResidencySetDescriptor* residencyDesc = [MTLResidencySetDescriptor new];
		id<MTLResidencySet> residency = [device newResidencySetWithDescriptor:residencyDesc error:&error];
		[queue addResidencySet:residency];
		[residency addAllocation:out];
		[residency commit];
		[residency requestResidency];
		MTL4ArgumentTableDescriptor* tableDesc = [MTL4ArgumentTableDescriptor new];
		tableDesc.maxBufferBindCount = 1;
		table = [device newArgumentTableWithDescriptor:tableDesc error:&error];
		allocator = [device newCommandAllocator];
		event = [device newSharedEvent];
	}
	uint64_t eventValue = 0;
	printf("compile: a pipeline of unique source after another, each dispatched once\n");
	fflush(stdout);

	NSDate* start = [NSDate date];
	double lastPrint = 0.0;
	uint64_t made = 0;
	uint64_t madeSincePrint = 0;
	while ([[NSDate date] timeIntervalSinceDate:start] < seconds && gErrors.load() == 0)
	{
		@autoreleasepool
		{
			// a kernel of 512 dependent operations whose constants no other kernel has
			std::string source = "#include <metal_stdlib>\nusing namespace metal;\nkernel void k(device float* out [[buffer(0)]], "
								 "uint tid [[thread_position_in_grid]])\n{\n\tfloat x = float(tid);\n";
			for (uint32_t opIt = 0; opIt < 512; opIt++)
				source += "\tx = fma(x, 1.0001f, " + std::to_string(made) + "." + std::to_string(opIt) + "f);\n";
			source += "\tif (x == 12345.0f)\n\t\tout[tid & 1023] = x;\n}\n";
			id<MTLComputePipelineState> pipeline = nil;
			if (deviceCompiler)
			{
				id<MTLLibrary> library = [device newLibraryWithSource:@(source.c_str()) options:[MTLCompileOptions new] error:&error];
				if (!library)
				{
					fprintf(stderr, "library: %s\n", error.localizedDescription.UTF8String);
					return 1;
				}
				pipeline = [device newComputePipelineStateWithFunction:[library newFunctionWithName:@"k"] error:&error];
			}
			else
			{
				MTL4LibraryDescriptor* libraryDesc = [MTL4LibraryDescriptor new];
				libraryDesc.source = @(source.c_str());
				libraryDesc.options = [MTLCompileOptions new];
				id<MTLLibrary> library = [compiler newLibraryWithDescriptor:libraryDesc error:&error];
				if (!library)
				{
					fprintf(stderr, "library: %s\n", error.localizedDescription.UTF8String);
					return 1;
				}
				MTL4LibraryFunctionDescriptor* function = [MTL4LibraryFunctionDescriptor new];
				function.name = @"k";
				function.library = library;
				MTL4ComputePipelineDescriptor* pipelineDesc = [MTL4ComputePipelineDescriptor new];
				pipelineDesc.computeFunctionDescriptor = function;
				pipeline = [compiler newComputePipelineStateWithDescriptor:pipelineDesc compilerTaskOptions:nil error:&error];
			}
			if (!pipeline)
			{
				fprintf(stderr, "pipeline: %s\n", error.localizedDescription.UTF8String);
				return 1;
			}
			if (!dispatch)
			{
				made++;
				madeSincePrint++;
				double now = [[NSDate date] timeIntervalSinceDate:start];
				if (now - lastPrint >= 1.0)
				{
					printf("%.0f s: %llu pipelines (%.1f a second), not dispatched\n", now, made, madeSincePrint / (now - lastPrint));
					fflush(stdout);
					lastPrint = now;
					madeSincePrint = 0;
				}
				continue;
			}
			if (classicQueue)
			{
				static id<MTLCommandQueue> classic = [device newCommandQueue];
				if (classicLast)
				{
					[classicLast waitUntilCompleted];
					if (classicLast.error)
					{
						gErrors++;
						fprintf(stderr, "command buffer error %ld: %s\n", (long)classicLast.error.code, classicLast.error.localizedDescription.UTF8String);
						break;
					}
				}
				id<MTLCommandBuffer> commandBuffer = [classic commandBuffer];
				id<MTLComputeCommandEncoder> encoder = [commandBuffer computeCommandEncoder];
				[encoder setComputePipelineState:pipeline];
				[encoder setBuffer:out offset:0 atIndex:0];
				[encoder dispatchThreads:MTLSizeMake(65536, 1, 1) threadsPerThreadgroup:MTLSizeMake(64, 1, 1)];
				[encoder endEncoding];
				[commandBuffer commit];
				classicLast = commandBuffer;
				made++;
				madeSincePrint++;
				double now = [[NSDate date] timeIntervalSinceDate:start];
				if (now - lastPrint >= 1.0)
				{
					printf("%.0f s: %llu pipelines (%.1f a second), classic queue\n", now, made, madeSincePrint / (now - lastPrint));
					fflush(stdout);
					lastPrint = now;
					madeSincePrint = 0;
				}
				continue;
			}
			if (eventValue > 0 && ![event waitUntilSignaledValue:eventValue timeoutMS:10000])
			{
				fprintf(stderr, "dispatch %llu not done within 10 s\n", made - 1);
				return 3;
			}
			[allocator reset];
			id<MTL4CommandBuffer> commandBuffer = [device newCommandBuffer];
			[commandBuffer beginCommandBufferWithAllocator:allocator];
			id<MTL4ComputeCommandEncoder> encoder = [commandBuffer computeCommandEncoder];
			[table setAddress:out.gpuAddress atIndex:0];
			[encoder setArgumentTable:table];
			[encoder setComputePipelineState:pipeline];
			[encoder dispatchThreads:MTLSizeMake(65536, 1, 1) threadsPerThreadgroup:MTLSizeMake(64, 1, 1)];
			[encoder endEncoding];
			[commandBuffer endCommandBuffer];
			MTL4CommitOptions* options = [MTL4CommitOptions new];
			[options addFeedbackHandler:^(id<MTL4CommitFeedback> feedback) {
				if (feedback.error)
				{
					gErrors++;
					fprintf(stderr, "commit error %ld: %s\n", (long)feedback.error.code, feedback.error.localizedDescription.UTF8String);
				}
			}];
			id<MTL4CommandBuffer> buffers[] = {commandBuffer};
			[queue commit:buffers count:1 options:options];
			[queue signalEvent:event value:++eventValue];
			made++;
			madeSincePrint++;
			double now = [[NSDate date] timeIntervalSinceDate:start];
			if (now - lastPrint >= 1.0)
			{
				printf("%.0f s: %llu pipelines (%.1f a second)\n", now, made, madeSincePrint / (now - lastPrint));
				fflush(stdout);
				lastPrint = now;
				madeSincePrint = 0;
			}
		}
	}
	if (event)
		[event waitUntilSignaledValue:eventValue timeoutMS:10000];
	if (classicLast)
		[classicLast waitUntilCompleted];
	printf("done: %llu pipelines, %d errors\n", made, gErrors.load());
	return gErrors.load() == 0 ? 0 : 4;
}

int main(int argc, const char** argv)
{
	@autoreleasepool
	{
		if (argc < 3)
		{
			fprintf(stderr, "usage: rtrepro trace <seconds> [rays] [threads] [rays per thread] | burn <seconds> [threads] [iterations] | hog <seconds> <gigabytes> | present <seconds> | stamp <seconds> [threads] [iterations] | compile <seconds> [dispatch] [mtl4|device] [mtl4|classic] | frame <seconds> [flags]\n");
			return 2;
		}
		if (strcmp(argv[1], "present") == 0)
			return RunPresent(atof(argv[2]));
		// frame <seconds> [flags: rtpudx by default, see RunFrame]
		if (strcmp(argv[1], "frame") == 0)
			return argc > 3 && strchr(argv[3], 'm') != nullptr ? RunFrameClassic(atof(argv[2]), argv[3]) : RunFrame(atof(argv[2]), argc > 3 ? argv[3] : "rtpudx");
		if (strcmp(argv[1], "compile") == 0)
			return RunCompile(
				atof(argv[2]), argc <= 3 || atoi(argv[3]) != 0, argc > 4 && strcmp(argv[4], "device") == 0, argc > 5 && strcmp(argv[5], "classic") == 0);
		bool trace = strcmp(argv[1], "trace") == 0;
		bool hog = strcmp(argv[1], "hog") == 0;
		bool stamp = strcmp(argv[1], "stamp") == 0;
		double seconds = atof(argv[2]);

		id<MTLDevice> device = MTLCreateSystemDefaultDevice();
		NSError* error = nil;
		id<MTLLibrary> library = [device newLibraryWithSource:@(kSource) options:nil error:&error];
		if (!library)
		{
			fprintf(stderr, "library: %s\n", error.localizedDescription.UTF8String);
			return 1;
		}
		id<MTLComputePipelineState> pipeline =
			[device newComputePipelineStateWithFunction:[library newFunctionWithName:trace ? @"trace" : @"burn"] error:&error];
		if (!pipeline)
		{
			fprintf(stderr, "pipeline: %s\n", error.localizedDescription.UTF8String);
			return 1;
		}

		id<MTL4CommandQueue> queue = [device newMTL4CommandQueue];
		MTLResidencySetDescriptor* residencyDesc = [MTLResidencySetDescriptor new];
		id<MTLResidencySet> residency = [device newResidencySetWithDescriptor:residencyDesc error:&error];
		[queue addResidencySet:residency];
		MTL4ArgumentTableDescriptor* tableDesc = [MTL4ArgumentTableDescriptor new];
		tableDesc.maxBufferBindCount = 4;
		std::vector<id<MTL4ArgumentTable>> tables;
		std::vector<id<MTL4CommandAllocator>> allocators;
		for (uint32_t frameIt = 0; frameIt < kFramesInFlight; frameIt++)
		{
			tables.push_back([device newArgumentTableWithDescriptor:tableDesc error:&error]);
			allocators.push_back([device newCommandAllocator]);
		}
		id<MTLSharedEvent> event = [device newSharedEvent];
		uint64_t eventValue = 0;

		auto commit = [&](id<MTL4CommandBuffer> commandBuffer)
		{
			MTL4CommitOptions* options = [MTL4CommitOptions new];
			[options addFeedbackHandler:^(id<MTL4CommitFeedback> feedback) {
				if (feedback.error)
				{
					gErrors++;
					fprintf(stderr, "commit error %ld: %s\n", (long)feedback.error.code, feedback.error.localizedDescription.UTF8String);
				}
			}];
			id<MTL4CommandBuffer> buffers[] = {commandBuffer};
			[queue commit:buffers count:1 options:options];
			[queue signalEvent:event value:++eventValue];
		};
		auto wait = [&](uint64_t value) -> bool { return [event waitUntilSignaledValue:value timeoutMS:10000]; };

		uint32_t threads = 0;
		uint32_t raysPerThread = 1;
		uint32_t iterations = 0;
		uint32_t rayCount = 0;
		id<MTLBuffer> rays = nil;
		id<MTLBuffer> counters = nil;
		id<MTLBuffer> params = nil;
		id<MTLBuffer> instances = nil;
		id<MTLBuffer> scratch = nil;
		std::vector<id<MTLAccelerationStructure>> instanceStructures;
		std::vector<id<MTLBuffer>> hogBuffers;
		// stamp: the counter heap (a ring of frames' timestamps) and the buffer it resolves into
		constexpr uint32_t kStampDispatchCount = 16;
		constexpr uint32_t kStampCount = 4096;
		id<MTL4CounterHeap> stampHeap = nil;
		id<MTLBuffer> stampBuffer = nil;
		MTL4InstanceAccelerationStructureDescriptor* instanceDesc = nil;

		if (trace)
		{
			std::vector<float> rayData = LoadRays(argc > 3 ? argv[3] : "rtrepro-rays.txt");
			rayCount = static_cast<uint32_t>(rayData.size() / 8);
			threads = argc > 4 ? static_cast<uint32_t>(atoi(argv[4])) : 65536;
			raysPerThread = argc > 5 ? static_cast<uint32_t>(atoi(argv[5])) : 8;
			rays = [device newBufferWithBytes:rayData.data() length:rayData.size() * sizeof(float) options:MTLResourceStorageModeShared];
			counters = [device newBufferWithLength:16 options:MTLResourceStorageModeShared];
			params = [device newBufferWithLength:256 * kFramesInFlight options:MTLResourceStorageModeShared];

			// a unit cube, 12 triangles
			const float vertices[8][3] = {{-0.5f, -0.5f, -0.5f}, {0.5f, -0.5f, -0.5f}, {0.5f, 0.5f, -0.5f}, {-0.5f, 0.5f, -0.5f},
										  {-0.5f, -0.5f, 0.5f},  {0.5f, -0.5f, 0.5f},  {0.5f, 0.5f, 0.5f},  {-0.5f, 0.5f, 0.5f}};
			const uint32_t indices[36] = {0, 2, 1, 0, 3, 2, 4, 5, 6, 4, 6, 7, 0, 1, 5, 0, 5, 4,
										  3, 6, 2, 3, 7, 6, 0, 4, 7, 0, 7, 3, 1, 2, 6, 1, 6, 5};
			id<MTLBuffer> vertexBuffer = [device newBufferWithBytes:vertices length:sizeof(vertices) options:MTLResourceStorageModeShared];
			id<MTLBuffer> indexBuffer = [device newBufferWithBytes:indices length:sizeof(indices) options:MTLResourceStorageModeShared];

			MTL4AccelerationStructureTriangleGeometryDescriptor* geometry = [MTL4AccelerationStructureTriangleGeometryDescriptor new];
			geometry.vertexBuffer = Range(vertexBuffer);
			geometry.vertexFormat = MTLAttributeFormatFloat3;
			geometry.vertexStride = sizeof(float) * 3;
			geometry.indexBuffer = Range(indexBuffer);
			geometry.indexType = MTLIndexTypeUInt32;
			geometry.triangleCount = 12;
			geometry.opaque = YES;
			MTL4PrimitiveAccelerationStructureDescriptor* primitiveDesc = [MTL4PrimitiveAccelerationStructureDescriptor new];
			primitiveDesc.geometryDescriptors = @[geometry];
			MTLAccelerationStructureSizes primitiveSizes = [device accelerationStructureSizesWithDescriptor:primitiveDesc];
			id<MTLAccelerationStructure> primitive = [device newAccelerationStructureWithSize:primitiveSizes.accelerationStructureSize];

			// the instances: the cube three times side by side (column major 4x3)
			instances = [device newBufferWithLength:sizeof(MTLIndirectAccelerationStructureInstanceDescriptor) * kInstanceCount
											options:MTLResourceStorageModeShared];
			auto* instanceData = static_cast<MTLIndirectAccelerationStructureInstanceDescriptor*>(instances.contents);
			for (uint32_t instanceIt = 0; instanceIt < kInstanceCount; instanceIt++)
			{
				MTLIndirectAccelerationStructureInstanceDescriptor& instance = instanceData[instanceIt];
				memset(&instance, 0, sizeof(instance));
				instance.transformationMatrix.columns[0] = MTLPackedFloat3Make(1.0f, 0.0f, 0.0f);
				instance.transformationMatrix.columns[1] = MTLPackedFloat3Make(0.0f, 1.0f, 0.0f);
				instance.transformationMatrix.columns[2] = MTLPackedFloat3Make(0.0f, 0.0f, 1.0f);
				instance.transformationMatrix.columns[3] = MTLPackedFloat3Make((static_cast<float>(instanceIt) - 1.0f) * 1.5f, 0.0f, 0.0f);
				instance.options = MTLAccelerationStructureInstanceOptionOpaque;
				instance.mask = 0xFF;
				instance.userID = instanceIt;
				instance.accelerationStructureID = primitive.gpuResourceID;
			}
			instanceDesc = [MTL4InstanceAccelerationStructureDescriptor new];
			instanceDesc.instanceDescriptorBuffer = Range(instances);
			instanceDesc.instanceDescriptorStride = sizeof(MTLIndirectAccelerationStructureInstanceDescriptor);
			instanceDesc.instanceCount = kInstanceCount;
			instanceDesc.instanceDescriptorType = MTLAccelerationStructureInstanceDescriptorTypeIndirect;
			MTLAccelerationStructureSizes instanceSizes = [device accelerationStructureSizesWithDescriptor:instanceDesc];
			for (uint32_t frameIt = 0; frameIt < kFramesInFlight; frameIt++)
				instanceStructures.push_back([device newAccelerationStructureWithSize:instanceSizes.accelerationStructureSize]);
			scratch = [device newBufferWithLength:std::max(primitiveSizes.buildScratchBufferSize, instanceSizes.buildScratchBufferSize) * kFramesInFlight
										  options:MTLResourceStorageModePrivate];

			for (id<MTLAllocation> allocation : std::vector<id<MTLAllocation>>{rays, counters, params, vertexBuffer, indexBuffer, instances, scratch, primitive})
				[residency addAllocation:allocation];
			for (id<MTLAccelerationStructure> structure : instanceStructures)
				[residency addAllocation:structure];
			[residency commit];
			[residency requestResidency];

			// the primitive structure, once
			id<MTL4CommandBuffer> commandBuffer = [device newCommandBuffer];
			[commandBuffer beginCommandBufferWithAllocator:allocators[0]];
			id<MTL4ComputeCommandEncoder> encoder = [commandBuffer computeCommandEncoder];
			[encoder buildAccelerationStructure:primitive descriptor:primitiveDesc scratchBuffer:Range(scratch)];
			[encoder endEncoding];
			[commandBuffer endCommandBuffer];
			commit(commandBuffer);
			if (!wait(eventValue))
			{
				fprintf(stderr, "the primitive structure's build didn't finish\n");
				return 1;
			}
			[allocators[0] reset];
			printf("trace: %u rays, %u threads of %u rays a frame, %u frames in flight\n", rayCount, threads, raysPerThread, kFramesInFlight);
		}
		else
		{
			threads = hog ? 65536 : (argc > 3 ? static_cast<uint32_t>(atoi(argv[3])) : 1u << 20);
			iterations = hog ? 200 : (argc > 4 ? static_cast<uint32_t>(atoi(argv[4])) : 2000);
			counters = [device newBufferWithLength:4096 options:MTLResourceStorageModeShared];
			params = [device newBufferWithLength:256 * kFramesInFlight options:MTLResourceStorageModeShared];
			[residency addAllocation:counters];
			[residency addAllocation:params];
			if (hog)
			{
				// in 256 MB buffers, every page touched
				constexpr uint64_t kChunkByteCount = 256ull << 20;
				uint64_t byteCount = static_cast<uint64_t>(atof(argc > 3 ? argv[3] : "2") * (1ull << 30));
				for (uint64_t allocated = 0; allocated < byteCount; allocated += kChunkByteCount)
				{
					id<MTLBuffer> buffer = [device newBufferWithLength:kChunkByteCount options:MTLResourceStorageModeShared];
					memset(buffer.contents, 0x5a, kChunkByteCount);
					[residency addAllocation:buffer];
					hogBuffers.push_back(buffer);
				}
				printf("hog: %.1f GB in %zu buffers, resident again every frame\n", static_cast<double>(byteCount) / (1ull << 30), hogBuffers.size());
			}
			[residency commit];
			[residency requestResidency];
			if (stamp)
			{
				MTL4CounterHeapDescriptor* heapDesc = [MTL4CounterHeapDescriptor new];
				heapDesc.type = MTL4CounterHeapTypeTimestamp;
				heapDesc.count = kStampCount;
				stampHeap = [device newCounterHeapWithDescriptor:heapDesc error:&error];
				if (!stampHeap)
				{
					fprintf(stderr, "counter heap: %s\n", error.localizedDescription.UTF8String);
					return 1;
				}
				stampBuffer = [device newBufferWithLength:kStampCount * sizeof(MTL4TimestampHeapEntry) options:MTLResourceStorageModeShared];
				[residency addAllocation:stampBuffer];
				[residency commit];
			}
			if (!hog)
				printf("%s: %u threads of %u iterations a frame%s\n", stamp ? "stamp" : "burn", threads, iterations,
					   stamp ? ", in 16 dispatches between timestamps" : "");
		}
		fflush(stdout);

		NSDate* start = [NSDate date];
		double lastPrint = 0.0;
		uint64_t frame = 0;
		uint64_t framesSincePrint = 0;
		while ([[NSDate date] timeIntervalSinceDate:start] < seconds && gErrors.load() == 0)
		{
			uint32_t slot = static_cast<uint32_t>(frame % kFramesInFlight);
			if (frame >= kFramesInFlight)
			{
				uint64_t value = eventValue - (kFramesInFlight - 1);
				if (!wait(value))
				{
					fprintf(stderr, "frame %llu not done within 10 s\n", frame - kFramesInFlight);
					return 3;
				}
				[allocators[slot] reset];
			}
			id<MTL4CommandBuffer> commandBuffer = [device newCommandBuffer];
			[commandBuffer beginCommandBufferWithAllocator:allocators[slot]];
			id<MTL4ComputeCommandEncoder> encoder = [commandBuffer computeCommandEncoder];
			id<MTL4ArgumentTable> table = tables[slot];
			if (trace)
			{
				// the frame's instance structure, rebuilt as speedo's is every frame, then the rays through it
				[encoder buildAccelerationStructure:instanceStructures[slot]
										 descriptor:instanceDesc
									  scratchBuffer:Range(scratch, scratch.length / kFramesInFlight * slot)];
				[encoder barrierAfterEncoderStages:MTLStageAccelerationStructure
							   beforeEncoderStages:MTLStageDispatch
								 visibilityOptions:MTL4VisibilityOptionNone];
				uint32_t traceParams[4] = {rayCount, raysPerThread, static_cast<uint32_t>(frame), 0};
				memcpy(static_cast<uint8_t*>(params.contents) + 256 * slot, traceParams, sizeof(traceParams));
				[table setAddress:rays.gpuAddress atIndex:0];
				[table setAddress:counters.gpuAddress atIndex:1];
				[table setAddress:params.gpuAddress + 256 * slot atIndex:2];
				[table setResource:instanceStructures[slot].gpuResourceID atBufferIndex:3];
			}
			else
			{
				uint32_t burnParams[4] = {iterations, 0, 0, 0};
				memcpy(static_cast<uint8_t*>(params.contents) + 256 * slot, burnParams, sizeof(burnParams));
				[table setAddress:counters.gpuAddress atIndex:0];
				[table setAddress:params.gpuAddress + 256 * slot atIndex:1];
			}
			[encoder setArgumentTable:table];
			[encoder setComputePipelineState:pipeline];
			uint32_t stampFirst = static_cast<uint32_t>(frame % (kStampCount / (2 * kStampDispatchCount))) * 2 * kStampDispatchCount;
			if (stamp)
			{
				for (uint32_t dispatchIt = 0; dispatchIt < kStampDispatchCount; dispatchIt++)
				{
					[encoder writeTimestampWithGranularity:MTL4TimestampGranularityRelaxed intoHeap:stampHeap atIndex:stampFirst + 2 * dispatchIt];
					[encoder dispatchThreads:MTLSizeMake(threads / kStampDispatchCount, 1, 1) threadsPerThreadgroup:MTLSizeMake(64, 1, 1)];
					[encoder writeTimestampWithGranularity:MTL4TimestampGranularityRelaxed intoHeap:stampHeap atIndex:stampFirst + 2 * dispatchIt + 1];
				}
			}
			else
				[encoder dispatchThreads:MTLSizeMake(threads, 1, 1) threadsPerThreadgroup:MTLSizeMake(64, 1, 1)];
			[encoder endEncoding];
			if (stamp)
				[commandBuffer resolveCounterHeap:stampHeap
										withRange:NSMakeRange(stampFirst, 2 * kStampDispatchCount)
									   intoBuffer:MTL4BufferRangeMake(stampBuffer.gpuAddress + stampFirst * sizeof(MTL4TimestampHeapEntry),
																	   2 * kStampDispatchCount * sizeof(MTL4TimestampHeapEntry))
										waitFence:nil
									  updateFence:nil];
			[commandBuffer endCommandBuffer];
			if (hog)
			{
				[residency commit];
				[residency requestResidency];
			}
			commit(commandBuffer);
			if (hog)
				usleep(8000);
			frame++;
			framesSincePrint++;
			double now = [[NSDate date] timeIntervalSinceDate:start];
			if (now - lastPrint >= 1.0)
			{
				printf("%.0f s: %llu frames (%.1f a second)%s\n", now, frame, framesSincePrint / (now - lastPrint),
					   trace ? (std::string(", hits ") + std::to_string(*static_cast<uint32_t*>(counters.contents))).c_str() : "");
				fflush(stdout);
				lastPrint = now;
				framesSincePrint = 0;
			}
		}
		wait(eventValue);
		printf("done: %llu frames, %d errors\n", frame, gErrors.load());
		return gErrors.load() == 0 ? 0 : 4;
	}
}
