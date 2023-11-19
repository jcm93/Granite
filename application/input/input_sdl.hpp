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

#pragma once
#include <SDL3/SDL.h>
#include "input.hpp"
#include <functional>

namespace Granite
{
class InputTrackerSDL
{
public:
	using Dispatcher = std::function<void (std::function<void ()>)>;
	bool init(InputTracker &tracker, const Dispatcher &dispatcher);
	void close();
	bool process_sdl_event(const SDL_Event &event, InputTracker &tracker, const Dispatcher &dispatcher);

private:
	SDL_Gamepad *pads[InputTracker::Joypads] = {};
	SDL_JoystickID ids[InputTracker::Joypads] = {};
	void add_gamepad(SDL_JoystickID id, InputTracker &tracker, const Dispatcher &dispatcher);
	void remove_gamepad(SDL_JoystickID id, InputTracker &tracker, const Dispatcher &dispatcher);
};
}
