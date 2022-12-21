/* Copyright (c) 2017-2022 Hans-Kristian Arntzen
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

#pragma once

#include "volatile_source.hpp"
#include "async_object_sink.hpp"
#include "image.hpp"

namespace Vulkan
{
class MemoryMappedTexture;
class Texture :
#ifndef GRANITE_SHIPPING
    public Granite::VolatileSource<Texture>,
#endif
    public Util::IntrusiveHashMapEnabled<Texture>
{
public:
	friend class Granite::VolatileSource<Texture>;
	friend class TextureManager;
	friend class Util::ObjectPool<Texture>;

	bool init_texture();
	void set_path(const std::string &path);
	Image *get_image();

#ifdef GRANITE_SHIPPING
	bool init();
#endif

private:
	Texture(Device *device, const std::string &path, VkFormat format = VK_FORMAT_UNDEFINED);

	explicit Texture(Device *device);

#ifdef GRANITE_SHIPPING
	std::string path;
#endif

	Device *device;
	Util::AsyncObjectSink<ImageHandle> handle;
	VkFormat format;
	void update_other(const void *data, size_t size);
	void update_gtx(Granite::FileMappingHandle file);
	void update_gtx(const MemoryMappedTexture &texture);
	void update_checkerboard();
	void load();
	void unload();
	void update(Granite::FileMappingHandle file);
	void replace_image(ImageHandle handle_);
};

class TextureManager
{
public:
	explicit TextureManager(Device *device);
	Texture *request_texture(const std::string &path, VkFormat format = VK_FORMAT_UNDEFINED);

	void init();

private:
	Device *device;
	VulkanCache<Texture> textures;
};
}
