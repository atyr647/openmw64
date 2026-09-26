// OpenMW-N64: walk around Morrowind's interior cells on a Nintendo 64.
//
// Game data is read by OpenMW's own components (ESM3 records, BSA archives,
// NIF meshes) cross-compiled for the VR4300. This file is the N64 front end:
// boot, a cell picker, and a free-fly camera. See README.md.

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <exception>
#include <fstream>
#include <iostream>
#include <memory>
#include <string>

#include <GL/gl.h>
#include <GL/gl_integration.h>
#include <libdragon.h>

#include <components/esm3/esmreader.hpp>

#include "datafiles.hpp"
#include "meshloader.hpp"
#include "scene.hpp"
#include "texture.hpp"
#include "worldindex.hpp"

namespace
{
    constexpr int sFont = 1;
    constexpr int sLineHeight = 11;
    constexpr int sMenuRows = 16;

    // Built with `make AUTOPLAY=1`: enter the first cell and look around
    // without input, for testing in emulators.
#ifdef OMW64_AUTOPLAY
    constexpr bool sAutoplay = true;
#else
    constexpr bool sAutoplay = false;
#endif

    int usedKb()
    {
        heap_stats_t stats;
        sys_get_heap_stats(&stats);
        return stats.used / 1024;
    }

    int totalKb()
    {
        heap_stats_t stats;
        sys_get_heap_stats(&stats);
        return stats.total / 1024;
    }

    template <class Draw>
    void textFrame(Draw&& draw)
    {
        surface_t* disp = display_get();
        rdpq_attach_clear(disp, nullptr);
        draw();
        rdpq_detach_show();
    }

    void text(int x, int y, const char* fmt, auto... args)
    {
        rdpq_text_printf(nullptr, sFont, static_cast<float>(x), static_cast<float>(y), fmt, args...);
    }

    void progressFrame(const char* title, const char* what, float fraction)
    {
        textFrame([&] {
            text(16, 90, "%s", title);
            text(16, 110, "%s", what);
            const int width = static_cast<int>(288 * std::clamp(fraction, 0.f, 1.f));
            rdpq_set_mode_fill(RGBA32(0x40, 0x40, 0x40, 0xff));
            rdpq_fill_rectangle(16, 124, 304, 134);
            rdpq_set_mode_fill(RGBA32(0xc0, 0x9a, 0x4a, 0xff));
            rdpq_fill_rectangle(16, 124, 16 + width, 134);
            text(16, 150, "RAM used: %d / %d KB", usedKb(), totalKb());
        });
    }

    [[noreturn]] void fatal(const std::string& message)
    {
        while (true)
        {
            textFrame([&] {
                text(16, 30, "OpenMW-N64 cannot continue:");
                rdpq_textparms_t parms{};
                parms.width = 288;
                parms.wrap = WRAP_WORD;
                rdpq_text_printf(&parms, sFont, 16, 50, "%s", message.c_str());
            });
        }
    }

    struct App
    {
        static constexpr std::size_t sEsmBufferSize = 64 * 1024;
        std::unique_ptr<char[]> mEsmBuffer = std::make_unique<char[]>(sEsmBufferSize);
        OMW64::DataFiles mData;
        ESM::ESMReader mEsm;
        OMW64::WorldIndex mIndex;
        std::unique_ptr<OMW64::TextureCache> mTextures;
        std::unique_ptr<OMW64::ModelCache> mModels;
        OMW64::CellScene mScene;
        OMW64::Renderer mRenderer;
        OMW64::Renderer::Camera mCamera;
        int mSelected = 0;

        void boot()
        {
            progressFrame("OpenMW-N64", "Looking for Morrowind data...", 0.f);
            if (!mData.init())
                fatal(
                    "No Morrowind data found. Copy the contents of 'Data Files' (Morrowind.esm and the .bsa "
                    "files) to the SD card folder 'Data Files'.");
            if (!is_memory_expanded())
                fatal("An Expansion Pak (8 MB RAM) is required.");

            mTextures = std::make_unique<OMW64::TextureCache>(mData);
            mModels = std::make_unique<OMW64::ModelCache>(mData, *mTextures);

            const std::string title = "Indexing " + mData.contentFile();
            // Big reads matter on a flashcart SD card: give the stream a 64 KB
            // buffer instead of the tiny default before OpenMW's reader uses it.
            auto stream = std::make_unique<std::ifstream>();
            stream->rdbuf()->pubsetbuf(mEsmBuffer.get(), sEsmBufferSize);
            stream->open(mData.root() + mData.contentFile(), std::ios::binary);
            if (!stream->is_open())
                fatal("Cannot open " + mData.root() + mData.contentFile());
            mEsm.open(std::move(stream), mData.root() + mData.contentFile());
            mIndex.scan(mEsm, [&](float f) { progressFrame(title.c_str(), "Reading cells and objects", f); });
            if (mIndex.interiors().empty())
                fatal(mData.contentFile() + " has no interior cells.");
        }

        void menu()
        {
            const auto& cells = mIndex.interiors();
            const int count = static_cast<int>(cells.size());
            for (int frame = 0;; ++frame)
            {
                if (sAutoplay && frame == 30)
                {
#ifdef OMW64_AUTOPLAY_CELL
                    for (int i = 0; i < count; ++i)
                        if (cells[i].mName == OMW64_AUTOPLAY_CELL)
                            mSelected = i;
#endif
                    return;
                }
                joypad_poll();
                const joypad_buttons_t pressed = joypad_get_buttons_pressed(JOYPAD_PORT_1);
                const joypad_buttons_t held = joypad_get_buttons_held(JOYPAD_PORT_1);
                int step = 0;
                if (pressed.d_down)
                    step = 1;
                if (pressed.d_up)
                    step = -1;
                if (pressed.d_right || pressed.c_down || (held.r && held.d_down))
                    step = sMenuRows;
                if (pressed.d_left || pressed.c_up || (held.r && held.d_up))
                    step = -sMenuRows;
                mSelected = (mSelected + step + count) % count;
                if (pressed.a || pressed.start)
                    return;

                const int top = std::clamp(mSelected - sMenuRows / 2, 0, std::max(0, count - sMenuRows));
                textFrame([&] {
                    text(12, 14, "OpenMW-N64  %s  (%d cells, %u objects)", mData.contentFile().c_str(), count,
                        static_cast<unsigned>(mIndex.objectCount()));
                    for (int row = 0; row < sMenuRows && top + row < count; ++row)
                    {
                        const int i = top + row;
                        text(
                            12, 34 + row * sLineHeight, "%c %.36s", i == mSelected ? '>' : ' ', cells[i].mName.c_str());
                    }
                    text(12, 222, "D-pad: choose   A: enter   RAM %d/%d KB", usedKb(), totalKb());
                });
            }
        }

        void loadCell()
        {
            // Free the previous cell before loading the next one.
            mScene = OMW64::CellScene();
            mModels->clear();
            mTextures->clear();

            const ESM::Cell& cell = mIndex.interiors()[mSelected];
            mScene.load(mEsm, cell, mIndex, *mModels,
                [&](float f, const char* what) { progressFrame(cell.mName.c_str(), what, f); });
            mCamera = OMW64::Renderer::Camera();
            mCamera.mPosition = mScene.mStart;
        }

        // Returns when START is pressed.
        void view()
        {
            float fps = 0.f;
            std::uint64_t last = get_ticks_us();
            bool showInfo = true;
            while (true)
            {
                if (sAutoplay)
                    mCamera.mYaw += 0.02f;
                joypad_poll();
                const joypad_inputs_t in = joypad_get_inputs(JOYPAD_PORT_1);
                const joypad_buttons_t pressed = joypad_get_buttons_pressed(JOYPAD_PORT_1);
                if (pressed.start)
                    return;
                if (pressed.b)
                    showInfo = !showInfo;

                const std::uint64_t now = get_ticks_us();
                const float dt = std::min(0.25f, static_cast<float>(now - last) / 1e6f);
                last = now;
                fps = fps * 0.9f + (dt > 0.f ? 0.1f / dt : 0.f);

                // Stick: walk / turn. C: look and strafe. Z/R: down/up. L: run.
                const float speed = (in.btn.l ? 1200.f : 400.f) * dt;
                const float stickX = std::abs(in.stick_x) > 8 ? in.stick_x / 80.f : 0.f;
                const float stickY = std::abs(in.stick_y) > 8 ? in.stick_y / 80.f : 0.f;
                mCamera.mYaw += stickX * 2.f * dt;
                const osg::Vec3f forward(std::sin(mCamera.mYaw), std::cos(mCamera.mYaw), 0.f);
                const osg::Vec3f right(forward.y(), -forward.x(), 0.f);
                mCamera.mPosition += forward * (stickY * speed);
                if (in.btn.c_left)
                    mCamera.mPosition -= right * speed;
                if (in.btn.c_right)
                    mCamera.mPosition += right * speed;
                if (in.btn.c_up)
                    mCamera.mPitch = std::min(1.4f, mCamera.mPitch + 1.5f * dt);
                if (in.btn.c_down)
                    mCamera.mPitch = std::max(-1.4f, mCamera.mPitch - 1.5f * dt);
                if (in.btn.r)
                    mCamera.mPosition.z() += speed;
                if (in.btn.z)
                    mCamera.mPosition.z() -= speed;

                surface_t* disp = display_get();
                surface_t* zbuf = display_get_zbuf();
                rdpq_attach(disp, zbuf);
                gl_context_begin();
                const OMW64::Renderer::Stats stats = mRenderer.draw(mScene, mCamera);
                gl_context_end();
                if (showInfo)
                {
                    text(8, 12, "%.34s", mScene.mName.c_str());
                    text(8, 24, "%.1f fps  %u objs  %u tris", fps, static_cast<unsigned>(stats.mDrawn),
                        static_cast<unsigned>(stats.mTriangles));
                    text(8, 232, "START: cells  B: hide info  RAM %d KB", usedKb());
                }
                rdpq_detach_show();
            }
        }

        void summary()
        {
            // What was (and was not) loaded, until A is pressed.
            for (int frame = 0;; ++frame)
            {
                if (sAutoplay && frame == 600)
                    return;
                joypad_poll();
                if (joypad_get_buttons_pressed(JOYPAD_PORT_1).a)
                    return;
                textFrame([&] {
                    text(16, 40, "%.36s", mScene.mName.c_str());
                    text(16, 64, "References:     %u", static_cast<unsigned>(mScene.mReferences));
                    text(16, 76, "Drawn objects:  %u", static_cast<unsigned>(mScene.mInstances.size()));
                    text(16, 88, "Meshes/Textures: %u / %u", static_cast<unsigned>(mModels->size()),
                        static_cast<unsigned>(mTextures->size()));
                    text(16, 100, "Triangles:      %u", static_cast<unsigned>(mScene.mTriangles));
                    text(16, 112, "Not drawn:      %u (NPCs, creatures, missing)",
                        static_cast<unsigned>(mScene.mMissingModels));
                    text(16, 124, "Out of memory:  %u", static_cast<unsigned>(mScene.mSkippedForMemory));
                    text(16, 136, "RAM used: %d / %d KB", usedKb(), totalKb());
                    text(16, 156, "Press A to walk around");

                    // The textures exactly as they were uploaded, 2x.
                    int x = 16;
                    mTextures->forEach([&](const std::string&, const OMW64::Texture& t) {
                        if (x + t.mSurface.width * 2 > 304)
                            return;
                        rdpq_blitparms_t parms{};
                        parms.scale_x = 2.f;
                        parms.scale_y = 2.f;
                        rdpq_set_mode_standard();
                        rdpq_tex_blit(&t.mSurface, x, 168, &parms);
                        x += t.mSurface.width * 2 + 4;
                    });
                });
            }
        }

        void run()
        {
            boot();
            mRenderer.init();
            while (true)
            {
                menu();
                if (mSelected < 0)
                    continue;
                loadCell();
                summary();
                view();
            }
        }
    };
}

int main()
{
    debug_init_isviewer();
    debug_init_usblog();
    debug_init_sdfs("sd:/", -1);
    // OpenMW's Log() writes to std::cout; libdragon forwards stderr to the
    // debug channels (emulator log / USB), so send it there.
    std::cout.rdbuf(std::cerr.rdbuf());
    dfs_init(DFS_DEFAULT_LOCATION);

    display_init(RESOLUTION_320x240, DEPTH_16_BPP, 3, GAMMA_NONE, FILTERS_RESAMPLE_ANTIALIAS_DEDITHER);
    rdpq_init();
    gl_init();
    joypad_init();
    rdpq_text_register_font(sFont, rdpq_font_load_builtin(FONT_BUILTIN_DEBUG_MONO));

    try
    {
        auto app = std::make_unique<App>(); // too big for the stack
        app->run();
    }
    catch (const std::exception& e)
    {
        fatal(e.what());
    }
    return 0;
}
