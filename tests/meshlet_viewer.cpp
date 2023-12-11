/* Copyright (c) 2017-2023 Hans-Kristian Arntzen
 *
 * Permission is hereby granted, free of charge, to any person obtaining
 * a copy of this software and associated documentation files (the
 * "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sublicense, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so, subject to
 * the following conditions:
 *
 * The above copyright notice and this permission notice shall be
 * included in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
 * IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY
 * CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
 * TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
 * SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 */

#define NOMINMAX
#include "application.hpp"
#include "command_buffer.hpp"
#include "scene_loader.hpp"
#include "device.hpp"
#include "os_filesystem.hpp"
#include "muglm/muglm_impl.hpp"
#include "meshlet.hpp"
#include "aabb.hpp"
#include "event.hpp"
#include "camera.hpp"
#include "event_manager.hpp"
#include "meshlet_export.hpp"
#include "render_context.hpp"
#include "gltf.hpp"
#include <string.h>
#include <float.h>
#include <stdexcept>

using namespace Granite;
using namespace Vulkan;
using namespace Vulkan::Meshlet;

static uint32_t style_to_u32_streams(MeshStyle style)
{
	switch (style)
	{
	case MeshStyle::Wireframe:
		return 3;
	case MeshStyle::Untextured:
		return 4;
	case MeshStyle::Textured:
		return 7;
	case MeshStyle::Skinned:
		return 9;
	default:
		return 0;
	}
}

struct MeshletRenderable : AbstractRenderable
{
	AssetID mesh;
	AABB aabb;

	void get_render_info(const RenderContext &, const RenderInfoComponent *,
	                     RenderQueue &) const override
	{
	}

	bool has_static_aabb() const override
	{
		return true;
	}

	const AABB *get_static_aabb() const override
	{
		return &aabb;
	}
};

struct MeshletViewerApplication : Granite::Application, Granite::EventHandler
{
	explicit MeshletViewerApplication(const char *path)
	{
		GLTF::Parser parser{path};

		std::vector<AssetID> mesh_assets;
		std::vector<NodeHandle> nodes;
		mesh_assets.reserve(parser.get_meshes().size());
		nodes.reserve(parser.get_nodes().size());

		unsigned count = 0;
		for (auto &mesh : parser.get_meshes())
		{
			auto internal_path = std::string("memory://mesh") + std::to_string(count++);
			if (!::Granite::Meshlet::export_mesh_to_meshlet(internal_path, mesh, MeshStyle::Textured))
				throw std::runtime_error("Failed to export meshlet.");
			mesh_assets.push_back(GRANITE_ASSET_MANAGER()->register_asset(
					*GRANITE_FILESYSTEM(), internal_path, Granite::AssetClass::Mesh));
		}

		for (auto &node : parser.get_nodes())
		{
			if (node.joint || node.has_skin)
			{
				nodes.emplace_back();
				continue;
			}

			auto nodeptr = scene.create_node();
			auto &node_transform = nodeptr->get_transform();
			node_transform.translation = node.transform.translation;
			node_transform.rotation = node.transform.rotation;
			node_transform.scale = node.transform.scale;
			nodes.push_back(std::move(nodeptr));
		}

		for (size_t i = 0, n = nodes.size(); i < n; i++)
		{
			auto &node = parser.get_nodes()[i];
			if (nodes[i])
			{
				for (auto &child : node.children)
					if (nodes[child])
						nodes[i]->add_child(nodes[child]);

				for (auto &mesh : node.meshes)
				{
					auto renderable = Util::make_handle<MeshletRenderable>();
					renderable->mesh = mesh_assets[mesh];
					renderable->aabb = parser.get_meshes()[mesh].static_aabb;
					scene.create_renderable(std::move(renderable), nodes[i].get());
				}
			}
		}

		auto &scene_nodes = parser.get_scenes()[parser.get_default_scene()];
		auto root = scene.create_node();
		for (auto &scene_node_index : scene_nodes.node_indices)
			root->add_child(nodes[scene_node_index]);
		scene.set_root_node(std::move(root));

		EVENT_MANAGER_REGISTER_LATCH(MeshletViewerApplication, on_device_create, on_device_destroy, DeviceCreatedEvent);
	}

	AABB aabb;
	FPSCamera camera;
	Scene scene;
	RenderContext render_context;
	VisibilityList list;

	void on_device_create(const DeviceCreatedEvent &e)
	{
		e.get_device().get_shader_manager().add_include_directory("builtin://shaders/inc");
	}

	void on_device_destroy(const DeviceCreatedEvent &)
	{
	}

	void render_frame(double, double) override
	{
		scene.update_all_transforms();

		auto &wsi = get_wsi();
		auto &device = wsi.get_device();
		auto cmd = device.request_command_buffer();

		list.clear();
		scene.gather_visible_opaque_renderables(render_context.get_visibility_frustum(), list);

		struct TaskParameters
		{
			uint32_t aabb_instance;
			uint32_t node_instance;
			uint32_t node_count; // Skinning
			uint32_t mesh_index_count;
		};

		struct DrawParameters
		{
			uint32_t node_instance;
			uint32_t node_count; // Skinning
		};

		std::vector<TaskParameters> task_params;

		for (auto &vis : list)
		{
			auto *meshlet = static_cast<const MeshletRenderable *>(vis.renderable);
			auto range = device.get_resource_manager().get_mesh_draw_range(meshlet->mesh);

			TaskParameters draw = {};
			draw.aabb_instance = vis.transform->aabb.offset;
			auto *node = vis.transform->scene_node;
			auto *skin = node->get_skin();
			draw.node_instance = skin ? skin->transform.offset : node->transform.offset;
			draw.node_count = skin ? skin->transform.count : 1;
			assert((range.offset & 31) == 0);

			for (uint32_t i = 0; i < range.count; i += 32)
			{
				draw.mesh_index_count = range.offset + i + (std::min(range.count - i, 32u) - 1);
				task_params.push_back(draw);
			}
		}

		if (task_params.empty())
		{
			cmd->begin_render_pass(device.get_swapchain_render_pass(SwapchainRenderPass::Depth));
			cmd->end_render_pass();
			device.submit(cmd);
			return;
		}

		BufferHandle task_buffer, cached_transform_buffer, aabb_buffer;
		auto &manager = device.get_resource_manager();

		{
			BufferCreateInfo info;
			info.size = task_params.size() * sizeof(task_params.front());
			info.domain = BufferDomain::LinkedDeviceHostPreferDevice;
			info.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
			task_buffer = device.create_buffer(info, task_params.data());
		}

		{
			BufferCreateInfo info;
			info.size = scene.get_transforms().get_count() * sizeof(*scene.get_transforms().get_cached_transforms());
			info.domain = BufferDomain::LinkedDeviceHostPreferDevice;
			info.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
			cached_transform_buffer = device.create_buffer(info, scene.get_transforms().get_cached_transforms());
		}

		{
			BufferCreateInfo info;
			info.size = scene.get_aabbs().get_count() * sizeof(*scene.get_aabbs().get_aabbs());
			info.domain = BufferDomain::LinkedDeviceHostPreferDevice;
			info.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
			aabb_buffer = device.create_buffer(info, scene.get_aabbs().get_aabbs());
		}

		if (manager.get_mesh_encoding() == Vulkan::ResourceManager::MeshEncoding::Meshlet)
		{
			auto *header_buffer = manager.get_meshlet_header_buffer();
			auto *stream_header_buffer = manager.get_meshlet_stream_header_buffer();
			auto *payload_buffer = manager.get_meshlet_payload_buffer();

			cmd->begin_render_pass(device.get_swapchain_render_pass(SwapchainRenderPass::Depth));
			camera.set_aspect(cmd->get_viewport().width / cmd->get_viewport().height);
			render_context.set_camera(camera);
			cmd->set_opaque_state();

			*cmd->allocate_typed_constant_data<mat4>(1, 0, 1) = render_context.get_render_parameters().view_projection;

			bool large_workgroup =
					device.get_device_features().mesh_shader_properties.maxPreferredMeshWorkGroupInvocations > 32 &&
					device.get_device_features().mesh_shader_properties.maxMeshWorkGroupInvocations >= 256;

			cmd->set_program("", "assets://shaders/meshlet_debug.mesh",
			                 "assets://shaders/meshlet_debug.mesh.frag",
			                 {{"MESHLET_PAYLOAD_LARGE_WORKGROUP", int(large_workgroup)}});

			cmd->set_storage_buffer(0, 0, *header_buffer);
			cmd->set_storage_buffer(0, 1, *stream_header_buffer);
			cmd->set_storage_buffer(0, 2, *payload_buffer);

			cmd->enable_subgroup_size_control(true, VK_SHADER_STAGE_MESH_BIT_EXT);
			cmd->set_subgroup_size_log2(true, 5, 5, VK_SHADER_STAGE_MESH_BIT_EXT);
			cmd->set_specialization_constant_mask(1);
			cmd->set_specialization_constant(0, style_to_u32_streams(MeshStyle::Textured));

			for (auto &draw : list)
			{
				auto *meshlet = static_cast<const MeshletRenderable *>(draw.renderable);
				auto range = device.get_resource_manager().get_mesh_draw_range(meshlet->mesh);
				if (range.count)
				{
					*cmd->allocate_typed_constant_data<mat4>(1, 1, 1) = draw.transform->get_world_transform();
					cmd->push_constants(&range.offset, 0, sizeof(range.offset));
					cmd->draw_mesh_tasks(range.count, 1, 1);
				}
			}
		}
		else
		{
			auto *ibo = manager.get_index_buffer();
			auto *pos = manager.get_position_buffer();
			auto *attr = manager.get_attribute_buffer();
			auto *indirect = manager.get_indirect_buffer();

			BufferHandle indirect_draws, compacted_params;
			{
				BufferCreateInfo info;
				info.size = task_params.size() * 32 * sizeof(VkDrawIndexedIndirectCommand) + 256;
				info.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT;
				info.domain = BufferDomain::Device;
				info.misc = BUFFER_MISC_ZERO_INITIALIZE_BIT;
				indirect_draws = device.create_buffer(info);
			}

			{
				BufferCreateInfo info;
				info.size = task_params.size() * 32 * sizeof(DrawParameters);
				info.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
				info.domain = BufferDomain::Device;
				compacted_params = device.create_buffer(info);
			}

			cmd->set_program("assets://shaders/meshlet_cull.comp");
			cmd->set_storage_buffer(0, 0, *aabb_buffer);
			cmd->set_storage_buffer(0, 1, *cached_transform_buffer);
			cmd->set_storage_buffer(0, 2, *task_buffer);
			cmd->set_storage_buffer(0, 3, *indirect);
			cmd->set_storage_buffer(0, 4, *indirect_draws);
			cmd->set_storage_buffer(0, 5, *compacted_params);
			uint32_t count = task_params.size();
			cmd->push_constants(&count, 0, sizeof(count));
			cmd->dispatch((count + 31) / 32, 1, 1);

			cmd->barrier(VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
			             VK_PIPELINE_STAGE_DRAW_INDIRECT_BIT | VK_PIPELINE_STAGE_VERTEX_SHADER_BIT |
			             VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
			             VK_ACCESS_INDIRECT_COMMAND_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_READ_BIT);

			cmd->begin_render_pass(device.get_swapchain_render_pass(SwapchainRenderPass::Depth));
			camera.set_aspect(cmd->get_viewport().width / cmd->get_viewport().height);
			render_context.set_camera(camera);
			cmd->set_opaque_state();

			*cmd->allocate_typed_constant_data<mat4>(1, 0, 1) = render_context.get_render_parameters().view_projection;

			cmd->set_program("assets://shaders/meshlet_debug.vert", "assets://shaders/meshlet_debug.frag");
			cmd->set_index_buffer(*ibo, 0, VK_INDEX_TYPE_UINT8_EXT);
			cmd->set_vertex_binding(0, *pos, 0, 12);
			cmd->set_vertex_binding(1, *attr, 0, 16);
			cmd->set_vertex_attrib(0, 0, VK_FORMAT_R32G32B32_SFLOAT, 0);
			cmd->set_vertex_attrib(1, 1, VK_FORMAT_A2B10G10R10_SNORM_PACK32, 0);
			cmd->set_vertex_attrib(2, 1, VK_FORMAT_A2B10G10R10_SNORM_PACK32, 4);
			cmd->set_vertex_attrib(3, 1, VK_FORMAT_R32G32_SFLOAT, 8);

			cmd->set_storage_buffer(0, 0, *compacted_params);
			cmd->set_storage_buffer(0, 1, *cached_transform_buffer);

			cmd->draw_indexed_multi_indirect(*indirect_draws,
			                                 256, task_params.size() * 32, sizeof(VkDrawIndexedIndirectCommand),
			                                 *indirect_draws, 0);
		}

		cmd->end_render_pass();
		device.submit(cmd);
	}
};

namespace Granite
{
Application *application_create(int argc, char **argv)
{
	GRANITE_APPLICATION_SETUP_FILESYSTEM();

	if (argc != 2)
	{
		LOGE("Usage: meshlet-viewer path.msh1\n");
		return nullptr;
	}

	try
	{
		auto *app = new MeshletViewerApplication(argv[1]);
		return app;
	}
	catch (const std::exception &e)
	{
		LOGE("application_create() threw exception: %s\n", e.what());
		return nullptr;
	}
}
}
