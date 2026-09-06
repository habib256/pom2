// POM2 Apple II Emulator
// Copyright (C) 2026 VERHILLE Arnaud
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program. If not, see <https://www.gnu.org/licenses/>.

// Voxel3DRenderer — the MicroM8-style 3D voxel view ("Voxel Cube" mode). It
// consumes the already-decoded, coloured Apple II framebuffer texture
// (whatever HiResMode / NTSC mode produced it) and rebuilds it as an UPRIGHT
// 4:3 grid of instanced cubes — each pixel a cube of the SAME thickness
// extruded toward the viewer (MicroM8 "Voxel Depth"), coloured by the pixel.
// Height is deliberately NOT luminance (that gives a spiky height-field, not
// MicroM8's flat slab); an optional per-colour Z push pops colours forward.
// An orbit camera (caller-supplied view-projection) looks at the grid.
//
// This is a *view-geometry* layer, orthogonal to the colour pipeline — it sits
// beside NtscPostProcessor / CrtEffectStack in MainWindow::drawScreenImage and
// follows the exact same lazy-init + FBO + GL-state-save/restore pattern. The
// only new GL infrastructure is a depth attachment (the 2D passes are depth-
// less) and instanced drawing. WebGL2/GLES3-friendly: no geometry shaders,
// instancing + vertex texture fetch + standard derivatives are all core there.

#ifndef POM2_VOXEL3D_RENDERER_H
#define POM2_VOXEL3D_RENDERER_H

#include "Mat4.h"

namespace pom2 {

class Voxel3DRenderer {
public:
    Voxel3DRenderer() = default;
    ~Voxel3DRenderer();

    Voxel3DRenderer(const Voxel3DRenderer&) = delete;
    Voxel3DRenderer& operator=(const Voxel3DRenderer&) = delete;

    /// Render `srcTex` (the coloured Apple II framebuffer; sampled by
    /// normalized UV, so its pixel size is irrelevant) as a voxel grid into an
    /// FBO sized `dstW`×`dstH`, viewed by `viewProj`. Returns the colour
    /// texture handle for ImGui::Image, or 0 on failure (caller falls back to
    /// the flat 2D blit).
    unsigned int process(unsigned int srcTex, int dstW, int dstH,
                         const Mat4& viewProj);

    bool available() const { return ready_; }

    /// Delete every GL object this renderer owns (FBO, colour texture,
    /// depth renderbuffer, VAO/VBO/EBO, shader program) and mark the
    /// renderer released. MUST be called while the GL context is still
    /// current; the destructor calls it too, which is safe because main()
    /// destroys its MainWindow — and hence this renderer — BEFORE
    /// ImGui_ImplOpenGL3_Shutdown / glfwDestroyWindow / glfwTerminate.
    /// Idempotent: a second call (or the destructor after an explicit one)
    /// issues no GL at all, so a host that tears the context down early can
    /// release here and let the destructor run wherever it likes.
    void releaseGL();

    // ── Tunables (Phase 3 will surface these in a panel) ──────────────────
    // MicroM8 "Voxel Cube" model: a 4:3 grid of equal-base-thickness cubes,
    // NOT a luminance height-field. `voxelDepth`/`colorShift` are in CELL-HEIGHT
    // units so the look is resolution-independent. `colorShift` is MicroM8's
    // per-colour forward "pop" (luminance-weighted); set 0 for a flat slab.
    // gridW/gridH default to native HGR but MainWindow overrides them per frame
    // with the live display resolution (280 or 560 × 192) → one voxel per pixel.
    int   gridW       = 280;    // cube columns (= display->width())
    int   gridH       = 192;    // cube rows    (= display->height())
    float voxelDepth  = 2.5f;   // base Z-thickness, in cell-height units
    float cubeFill    = 1.0f;   // cell fraction; 1.0 = contiguous (no gap grid)
    float colorShift  = 8.0f;   // per-colour forward pop, in cell-height units
    bool  mono        = false;  // "Voxel Cube Mono" — grey output, relief kept
    bool  perColorDepth = false;// depth from nearest palette index (vs luminance)
    float ambient     = 0.5f;   // lighting floor so no face is pure black
    int   superSample = 3;      // FBO render scale; minify-downsampled (anti-alias)
    float bg[3]       = { 0.05f, 0.05f, 0.08f };  // clear colour

private:
    bool initialize();
    bool createTargets(int w, int h);

    bool ready_ = false, initTried_ = false;
    unsigned int program_  = 0;
    unsigned int vao_ = 0, vbo_ = 0, ebo_ = 0;
    unsigned int fbo_ = 0, colorTex_ = 0, depthRb_ = 0;
    int texW_ = 0, texH_ = 0;

    int uViewProj_ = -1, uTex_ = -1, uGrid_ = -1, uCell_ = -1,
        uDepth_ = -1, uFill_ = -1, uColorShift_ = -1, uMono_ = -1,
        uDepthMode_ = -1, uPalette_ = -1, uLightDir_ = -1, uAmbient_ = -1;
};

}  // namespace pom2

#endif  // POM2_VOXEL3D_RENDERER_H
