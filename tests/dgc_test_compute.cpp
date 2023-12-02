#define NOMINMAX
#include "application.hpp"
#include "command_buffer.hpp"
#include "device.hpp"
#include "os_filesystem.hpp"
#include "cli_parser.hpp"
#include <string.h>

using namespace Granite;
using namespace Vulkan;

struct Options
{
	uint32_t max_count = 1;
	uint32_t indirect_count = 1;
	uint32_t iterations = 1;
	VkDispatchIndirectCommand dispatch = { 1, 1, 1 };
	uint32_t frames = 1000;
	bool use_indirect_count = false;
	bool use_indirect = false;
	bool use_mdi = false;
	bool use_dgcc = false;
	bool async = false;
};

struct DGCTriangleApplication : Granite::Application, Granite::EventHandler
{
	explicit DGCTriangleApplication(const Options &options_)
		: options(options_)
	{
		EVENT_MANAGER_REGISTER_LATCH(DGCTriangleApplication, on_device_created, on_device_destroyed, DeviceCreatedEvent);
	}

	Options options;
	const IndirectLayout *indirect_layout = nullptr;
	Vulkan::BufferHandle dgc_buffer;
	Vulkan::BufferHandle dgc_count_buffer;
	Vulkan::BufferHandle ssbo;
	Vulkan::BufferHandle ssbo_readback;
	uint32_t frame_count = 0;
	bool has_renderdoc = false;

	struct DGC
	{
		uint32_t push;
		VkDispatchIndirectCommand dispatch;
	};

	void on_device_created(const DeviceCreatedEvent &e)
	{
		IndirectLayoutToken tokens[2] = {};

		{
			BufferCreateInfo buf_info = {};
			buf_info.domain = BufferDomain::Device;
			buf_info.size = options.max_count * sizeof(uint32_t);
			buf_info.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
			buf_info.misc = BUFFER_MISC_ZERO_INITIALIZE_BIT;
			ssbo = e.get_device().create_buffer(buf_info, nullptr);
			buf_info.domain = BufferDomain::CachedHost;
			buf_info.misc = 0;
			ssbo_readback = e.get_device().create_buffer(buf_info, nullptr);
		}

		auto *layout = e.get_device().get_shader_manager().register_compute(
				"assets://shaders/dgc_compute.comp")->
				register_variant({})->get_program()->get_pipeline_layout();

		tokens[0].type = IndirectLayoutToken::Type::PushConstant;
		tokens[0].offset = offsetof(DGC, push);
		tokens[0].data.push.range = 4;
		tokens[0].data.push.offset = 0;
		tokens[0].data.push.layout = layout;
		tokens[1].type = IndirectLayoutToken::Type::Dispatch;
		tokens[1].offset = offsetof(DGC, dispatch);

		if (e.get_device().get_device_features().device_generated_commands_compute_features.deviceGeneratedCompute)
			indirect_layout = e.get_device().request_indirect_layout(tokens, 2, sizeof(DGC));

		std::vector<DGC> dgc_data(options.max_count);
		for (unsigned i = 0; i < options.max_count; i++)
		{
			auto &dgc = dgc_data[i];
			dgc.push = i;
			dgc.dispatch = options.dispatch;
		}

		BufferCreateInfo buf_info = {};
		buf_info.domain = BufferDomain::LinkedDeviceHost;
		buf_info.size = dgc_data.size() * sizeof(dgc_data.front());
		buf_info.usage = VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT;
		dgc_buffer = e.get_device().create_buffer(buf_info, dgc_data.data());

		const uint32_t count_data[] = { options.indirect_count };
		buf_info.size = sizeof(count_data);
		dgc_count_buffer = e.get_device().create_buffer(buf_info, &options.indirect_count);

		has_renderdoc = Device::init_renderdoc_capture();
	}

	void on_device_destroyed(const DeviceCreatedEvent &)
	{
		dgc_buffer.reset();
		dgc_count_buffer.reset();
		ssbo.reset();
		ssbo_readback.reset();
		indirect_layout = nullptr;
	}

	void render_frame(double, double) override
	{
		auto &wsi = get_wsi();
		auto &device = wsi.get_device();

		if (options.use_dgcc && !device.get_device_features().device_generated_commands_compute_features.deviceGeneratedCompute)
		{
			LOGE("DGCC is not supported.\n");
			request_shutdown();
			return;
		}

		if (has_renderdoc && frame_count == 0)
			device.begin_renderdoc_capture();

		uint32_t num_threads = 0;
		auto cmd = device.request_command_buffer(options.async ?
		                                         CommandBuffer::Type::AsyncCompute :
		                                         CommandBuffer::Type::Generic);
		cmd->set_storage_buffer(0, 0, *ssbo);
		cmd->set_program("assets://shaders/dgc_compute.comp");
		auto start_ts = cmd->write_timestamp(VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT);
		for (uint32_t i = 0; i < options.iterations; i++)
		{
			uint32_t indirect_dispatch_count = options.use_indirect_count ?
			                                   std::min(options.indirect_count, options.max_count) :
			                                   options.max_count;

			if (options.use_dgcc)
			{
				cmd->execute_indirect_commands(indirect_layout, options.max_count, *dgc_buffer, 0,
				                               options.use_indirect_count ? dgc_count_buffer.get() : nullptr, 0);
			}
			else if (options.use_indirect)
			{
				for (uint32_t j = 0; j < indirect_dispatch_count; j++)
				{
					cmd->push_constants(&j, 0, sizeof(j));
					cmd->dispatch_indirect(*dgc_buffer, offsetof(DGC, dispatch) + j * sizeof(DGC));
				}
			}
			else
			{
				for (uint32_t j = 0; j < indirect_dispatch_count; j++)
					cmd->dispatch(options.dispatch.x, options.dispatch.y, options.dispatch.z);
			}
			num_threads += indirect_dispatch_count * options.dispatch.x * options.dispatch.y * options.dispatch.z * 32;
		}
		auto end_ts = cmd->write_timestamp(VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT);
		device.submit(cmd);
		device.register_time_interval("GPU", std::move(start_ts), std::move(end_ts), "Shading");

		if (has_renderdoc && frame_count == 0)
			device.end_renderdoc_capture();

		cmd = device.request_command_buffer();
		cmd->begin_render_pass(device.get_swapchain_render_pass(SwapchainRenderPass::ColorOnly));
		cmd->end_render_pass();
		device.submit(cmd);

		LOGI("Ran frame!\n");
		if (++frame_count >= options.frames)
		{
			request_shutdown();
			device.timestamp_log([num_threads](const std::string &, const TimestampIntervalReport &report) {
				printf("%.3f ms / frame\n", 1e3 * report.time_per_frame_context);
				printf("%.3f ns / compute thread\n", 1e9 * report.time_per_frame_context / double(3 * num_threads));
			});
		}
	}
};

namespace Granite
{
static void print_help()
{
	LOGI("Usage: dgc-test-graphics\n"
	     "\t[--max-count (maxSequenceCount / maxDraws)]\n"
	     "\t[--indirect-count (indirect count placed in indirect buffer)]\n"
	     "\t[--iterations (iterations)]\n"
	     "\t[--indirect (use indirect draw)]\n"
	     "\t[--dispatch (number of workgroups))]\n"
	     "\t[--dgcc (use NV_dgcc)]\n"
	     "\t[--async (use async compute)]\n"
	     "\t[--frames (number of frames to render before exiting)]\n");
}

Application *application_create(int argc, char **argv)
{
	GRANITE_APPLICATION_SETUP_FILESYSTEM();
	Options options;

	Util::CLICallbacks cbs;
	cbs.add("--max-count", [&](Util::CLIParser &parser) {
		options.max_count = parser.next_uint();
	});
	cbs.add("--indirect-count", [&](Util::CLIParser &parser) {
		options.indirect_count = parser.next_uint();
		options.use_indirect_count = true;
		options.use_indirect = true;
	});
	cbs.add("--iterations", [&](Util::CLIParser &parser) {
		options.iterations = parser.next_uint();
	});
	cbs.add("--indirect", [&](Util::CLIParser &) {
		options.use_indirect = true;
	});
	cbs.add("--dispatch", [&](Util::CLIParser &parser) {
		options.dispatch.x = parser.next_uint();
	});
	cbs.add("--dgcc", [&](Util::CLIParser &) {
		options.use_dgcc = true;
		options.use_indirect = true;
	});
	cbs.add("--frames", [&](Util::CLIParser &parser) {
		options.frames = parser.next_uint();
	});
	cbs.add("--async", [&](Util::CLIParser &) {
		options.async = true;
	});
	cbs.add("--help", [&](Util::CLIParser &parser) {
		parser.end();
	});

	Util::CLIParser parser(std::move(cbs), argc - 1, argv + 1);
	if (!parser.parse() || parser.is_ended_state())
	{
		print_help();
		return nullptr;
	}

	try
	{
		auto *app = new DGCTriangleApplication(options);
		return app;
	}
	catch (const std::exception &e)
	{
		LOGE("application_create() threw exception: %s\n", e.what());
		return nullptr;
	}
}
}
