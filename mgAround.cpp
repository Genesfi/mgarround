/*******************************************************************/
/*                                                                 */
/*                      ADOBE CONFIDENTIAL                         */
/*                   _ _ _ _ _ _ _ _ _ _ _ _ _                     */
/*                                                                 */
/* Copyright 2007-2023 Adobe Inc.                                  */
/* All Rights Reserved.                                            */
/*                                                                 */
/* NOTICE:  All information contained herein is, and remains the   */
/* property of Adobe Inc. and its suppliers, if                    */
/* any.  The intellectual and technical concepts contained         */
/* herein are proprietary to Adobe Inc. and its                    */
/* suppliers and may be covered by U.S. and Foreign Patents,       */
/* patents in process, and are protected by trade secret or        */
/* copyright law.  Dissemination of this information or            */
/* reproduction of this material is strictly forbidden unless      */
/* prior written permission is obtained from Adobe Inc.            */
/* Incorporated.                                                   */
/*                                                                 */
/*******************************************************************/

/*	mgAround.cpp
 */

#include "mgAround.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>
#include <map>
#include <mutex>
#include <sstream>
#include <thread>
#include <utility>
#include <vector>

static AEGP_PluginID s_my_aegp_id = 0;

// ---------------------------------------------------------------------
// OPTIMIZATION #2: Debug logging is now compiled out entirely in Release
// builds. write_log() previously opened a file and took a mutex lock on
// every call inside the render path (once per frame when mb_enable ==
// "Comp Setting"), which is pure overhead in production. Use MGA_LOG(x)
// everywhere instead of calling write_log directly.
// ---------------------------------------------------------------------
#if defined(MGA_ENABLE_DEBUG_LOG)
static std::mutex log_mutex;
inline void write_log(const std::string& msg) {
    std::lock_guard<std::mutex> lock(log_mutex);
    std::ofstream f("C:\\Users\\Gusti "
        "N\\.gemini\\antigravity-ide\\brain\\767a8f19-7caf-4d8d-b666-"
        "d6b89e1b80be\\ae_log.txt",
        std::ios::app);
    if (f.is_open()) {
        f << msg << "\n";
    }
}
#define MGA_LOG(x) write_log(x)
#else
// Keep logging (and construction of its string arguments) out of production
// renders. Define MGA_ENABLE_DEBUG_LOG only for diagnostic builds.
#define MGA_LOG(x) do { } while (0)
#endif

#ifndef min
#define min(a, b) (((a) < (b)) ? (a) : (b))
#endif

#ifndef max
#define max(a, b) (((a) > (b)) ? (a) : (b))
#endif

struct BBox {
    double min_x, max_x;
    double min_y, max_y;
    double cx, cy;
    double max_alpha;
};

struct UnionFind {
    std::vector<int> parent;
    UnionFind(int n) {
        parent.resize(n);
        for (int i = 0; i < n; ++i)
            parent[i] = i;
    }
    int find(int i) {
        while (i != parent[i]) {
            parent[i] = parent[parent[i]]; // path compression
            i = parent[i];
        }
        return i;
    }
    void unite(int i, int j) {
        int root_i = find(i);
        int root_j = find(j);
        if (root_i != root_j) {
            parent[root_i] = root_j;
        }
    }
};

struct BBoxDouble {
    double min_x, max_x;
    double min_y, max_y;
};

// ---------------------------------------------------------------------
// OPTIMIZATION #1: Precomputed per-box geometry.
// GetBoxCoverage() used to recompute box_w, box_h, box_cx, box_cy,
// half_w, half_h, clamped_r, bound_x, bound_y for every box on every
// single pixel (O(width * height * num_boxes) redundant math, including
// divisions and abs() calls). None of these values depend on the pixel
// being evaluated -- only on the box and the (per-render-call-constant)
// refcon settings. We now compute them once per box, per sample, before
// the pixel loop starts.
// ---------------------------------------------------------------------
struct PrecomputedBox {
    double box_cx, box_cy;
    double geom_cx, geom_cy;
    double box_w, box_h;
    double half_w, half_h;
    double clamped_r;
    double bound_x, bound_y;
};

inline void PrecomputeBoxGeometry(const std::vector<BBox>& boxes,
    const struct RenderRefcon* refcon,
    std::vector<PrecomputedBox>& out);

template <typename PixelType>
BBoxDouble FindContentBounds(const PF_EffectWorld* world) {
    int min_x = world->width;
    int max_x = -1;
    int min_y = world->height;
    int max_y = -1;

    for (int y = 0; y < world->height; ++y) {
        PixelType* row = (PixelType*)((char*)world->data + y * world->rowbytes);
        for (int x = 0; x < world->width; ++x) {
            if (row[x].alpha > 0) {
                if (x < min_x)
                    min_x = x;
                if (x > max_x)
                    max_x = x;
                if (y < min_y)
                    min_y = y;
                if (y > max_y)
                    max_y = y;
            }
        }
    }

    BBoxDouble bounds;
    if (max_x < 0) {
        bounds.min_x = 0;
        bounds.max_x = world->width - 1;
        bounds.min_y = 0;
        bounds.max_y = world->height - 1;
    }
    else {
        bounds.min_x = min_x;
        bounds.max_x = max_x;
        bounds.min_y = min_y;
        bounds.max_y = max_y;
    }
    return bounds;
}

inline BBoxDouble GetWorldContentBounds(const PF_EffectWorld* world,
    bool is_deep) {
    if (is_deep) {
        return FindContentBounds<PF_Pixel16>(world);
    }
    else {
        return FindContentBounds<PF_Pixel8>(world);
    }
}

struct SampleData {
    bool has_layer_transform;
    double layer_cx;
    double layer_cy;
    double cos_total_theta;
    double sin_total_theta;
    double animator_rotation;
    double sample_theta;
    double cos_sample_theta;
    double sin_sample_theta;
    std::vector<BBox> boxes;
    std::vector<PrecomputedBox> precomputed; // OPTIMIZATION #1
    // For non-repeater renders, only test boxes that can intersect the current
    // row. This avoids O(pixels * glyphs) work in Per Character mode.
    std::vector<std::vector<size_t>> row_box_candidates;
    bool has_custom_layer;
    PF_EffectWorld custom_world;
    BBoxDouble custom_bounds;
    PF_EffectWorld input_world;
};

struct RenderRefcon {
    int mode;
    int target;
    PF_Pixel box_color;
    double box_opacity;
    PF_Pixel outline_color;
    double outline_opacity;
    double render_stroke_width;
    double render_border_radius;
    double render_pad_left;
    double render_pad_right;
    double render_pad_top;
    double render_pad_bottom;
    double scale_x_pct;
    double scale_y_pct;
    double render_offset_x;
    double render_offset_y;
    double theta;
    double cos_theta;
    double sin_theta;
    bool keep_aspect;
    int comp_mode;
    bool fill_outline;
    PF_EffectWorld* input;
    PF_EffectWorld* output;

    int num_samples;
    int num_valid_samples;
    std::vector<SampleData> samples;

    bool consistent_size;
    double max_char_w;
    double max_char_h;

    double render_min_x;
    double render_max_x;
    double render_min_y;
    double render_max_y;

    bool repeater_enable;
    int repeater_count_l;
    int repeater_count_r;
    int repeater_count_u;
    int repeater_count_d;
    double repeater_gap_x;
    double repeater_gap_y;
    double repeater_offset_x;
    double repeater_offset_y;
    bool repeater_show_text;
    bool repeater_show_shape;
    double global_cx;
    double global_cy;
    double repeater_trans_x;
    double repeater_trans_y;
    double repeater_trans_cos;
    double repeater_trans_sin;
    bool has_layer_transform;
    double layer_cx;
    double layer_cy;
    double total_theta;
    double cos_total_theta;
    double sin_total_theta;
};

inline void PrecomputeBoxGeometry(const std::vector<BBox>& boxes,
    const RenderRefcon* refcon,
    double cos_t_param,
    double sin_t_param,
    std::vector<PrecomputedBox>& out) {
    out.resize(boxes.size());

    double cos_t = abs(cos_t_param);
    double sin_t = abs(sin_t_param);

    for (size_t i = 0; i < boxes.size(); ++i) {
        const BBox& box = boxes[i];
        PrecomputedBox& pb = out[i];

        double W = box.max_x - box.min_x + 1;
        double H = box.max_y - box.min_y + 1;
        double cx = (box.min_x + box.max_x) / 2.0;
        double cy = (box.min_y + box.max_y) / 2.0;

        double W_eval = refcon->consistent_size ? refcon->max_char_w : W;
        double H_eval = refcon->consistent_size ? refcon->max_char_h : H;

        pb.box_w = (W_eval + refcon->render_pad_left + refcon->render_pad_right) *
            (refcon->scale_x_pct / 100.0);
        pb.box_h = (H_eval + refcon->render_pad_top + refcon->render_pad_bottom) *
            (refcon->scale_y_pct / 100.0);

        // Geser titik tengah box sesuai selisih padding kiri/kanan & atas/bawah,
        // supaya nambah pad_right cuma melebarkan ke kanan, bukan dua-duanya.
        double pad_shift_x = (refcon->render_pad_right - refcon->render_pad_left) *
            (refcon->scale_x_pct / 100.0) / 2.0;
        double pad_shift_y = (refcon->render_pad_bottom - refcon->render_pad_top) *
            (refcon->scale_y_pct / 100.0) / 2.0;

        pb.box_cx = cx + refcon->render_offset_x + pad_shift_x;
        pb.box_cy = cy + refcon->render_offset_y + pad_shift_y;

        pb.half_w = pb.box_w / 2.0;
        pb.half_h = pb.box_h / 2.0;

        double r = refcon->render_border_radius;
        double clamped_r = min(r, min(pb.half_w, pb.half_h));
        if (clamped_r < 0.0)
            clamped_r = 0.0;
        pb.clamped_r = clamped_r;

        double pad_val = clamped_r + refcon->render_stroke_width + 4.0;
        pb.bound_x = pb.half_w * cos_t + pb.half_h * sin_t + pad_val;
        pb.bound_y = pb.half_w * sin_t + pb.half_h * cos_t + pad_val;
    }
}

inline void BuildRowBoxCandidates(SampleData& sample) {
    const int height = sample.input_world.height;
    sample.row_box_candidates.clear();
    if (height <= 0 || sample.precomputed.empty()) {
        return;
    }

    sample.row_box_candidates.resize(height);
    for (size_t bi = 0; bi < sample.precomputed.size(); ++bi) {
        const PrecomputedBox& pb = sample.precomputed[bi];
        int first_y = (int)floor(pb.box_cy - pb.bound_y) - 1;
        int last_y = (int)ceil(pb.box_cy + pb.bound_y) + 1;
        if (first_y < 0) first_y = 0;
        if (last_y >= height) last_y = height - 1;
        for (int y = first_y; y <= last_y; ++y) {
            sample.row_box_candidates[y].push_back(bi);
        }
    }
}

inline double clamped_channel(double val, double max_val) {
    if (val < 0.0)
        return 0.0;
    if (val > max_val)
        return max_val;
    return val;
}

template <typename T, typename P>
inline void sample_bilinear_xy(const PF_EffectWorld* world, double x, double y,
    double* r, double* g, double* b, double* a,
    double max_val) {
    if (x < 0)
        x = 0;
    if (x > world->width - 1)
        x = world->width - 1;
    if (y < 0)
        y = 0;
    if (y > world->height - 1)
        y = world->height - 1;

    int x0 = (int)x;
    int y0 = (int)y;
    int x1 = min(x0 + 1, (int)world->width - 1);
    int y1 = min(y0 + 1, (int)world->height - 1);

    double tx = x - x0;
    double ty = y - y0;

    T* row0 = (T*)((char*)world->data + y0 * world->rowbytes);
    T* row1 = (T*)((char*)world->data + y1 * world->rowbytes);

    P p00 = row0[x0];
    P p10 = row0[x1];
    P p01 = row1[x0];
    P p11 = row1[x1];

    double w00 = (1.0 - tx) * (1.0 - ty);
    double w10 = tx * (1.0 - ty);
    double w01 = (1.0 - tx) * ty;
    double w11 = tx * ty;

    *a = (p00.alpha * w00 + p10.alpha * w10 + p01.alpha * w01 + p11.alpha * w11) /
        max_val;
    *r =
        (p00.red * w00 + p10.red * w10 + p01.red * w01 + p11.red * w11) / max_val;
    *g = (p00.green * w00 + p10.green * w10 + p01.green * w01 + p11.green * w11) /
        max_val;
    *b = (p00.blue * w00 + p10.blue * w10 + p01.blue * w01 + p11.blue * w11) /
        max_val;
}

template <typename T, typename P>
inline void sample_bilinear(const PF_EffectWorld* world, double u, double v,
    double* r, double* g, double* b, double* a,
    double max_val) {
    sample_bilinear_xy<T, P>(world, u * (world->width - 1),
        v * (world->height - 1), r, g, b, a, max_val);
}

template <typename T>
inline double GetAlphaAt(const PF_EffectWorld* world, double x, double y,
    double max_val) {
    // Return 0 for out-of-bounds coordinates instead of clamping to edge pixels
    if (x < -0.5 || x > world->width - 0.5 || y < -0.5 || y > world->height - 0.5)
        return 0.0;

    if (x < 0)
        x = 0;
    if (x > world->width - 1)
        x = world->width - 1;
    if (y < 0)
        y = 0;
    if (y > world->height - 1)
        y = world->height - 1;

    int x0 = (int)x;
    int y0 = (int)y;
    int x1 = min(x0 + 1, (int)world->width - 1);
    int y1 = min(y0 + 1, (int)world->height - 1);

    double tx = x - x0;
    double ty = y - y0;

    T* row0 = (T*)((char*)world->data + y0 * world->rowbytes);
    T* row1 = (T*)((char*)world->data + y1 * world->rowbytes);

    double a00 = row0[x0].alpha / max_val;
    double a10 = row0[x1].alpha / max_val;
    double a01 = row1[x0].alpha / max_val;
    double a11 = row1[x1].alpha / max_val;

    double w00 = (1.0 - tx) * (1.0 - ty);
    double w10 = tx * (1.0 - ty);
    double w01 = (1.0 - tx) * ty;
    double w11 = tx * ty;

    return a00 * w00 + a10 * w10 + a01 * w01 + a11 * w11;
}

inline double GetTextAlphaBilinear(const PF_EffectWorld* world, double x,
    double y, bool is_deep) {
    if (is_deep) {
        return GetAlphaAt<PF_Pixel16>(world, x, y, 32768.0);
    }
    else {
        return GetAlphaAt<PF_Pixel8>(world, x, y, 255.0);
    }
}

inline double GetMaxAlphaInNeighborhood(double ox, double oy, double rx,
    double ry, const PF_EffectWorld* world,
    bool is_deep) {
    int rx_pixels = (int)ceil(rx);
    int ry_pixels = (int)ceil(ry);

    // Check center first to enable instant short-circuiting for pixels inside the
    // text
    double center_a = GetTextAlphaBilinear(world, ox, oy, is_deep);
    if (center_a >= 0.999) {
        return 1.0;
    }
    double max_alpha = center_a;

    for (int dy = -ry_pixels; dy <= ry_pixels; ++dy) {
        for (int dx = -rx_pixels; dx <= rx_pixels; ++dx) {
            if (dx == 0 && dy == 0)
                continue; // Already checked center

            double n_x = ox + dx;
            double n_y = oy + dy;
            if (n_x >= 0.0 && n_x < world->width && n_y >= 0.0 &&
                n_y < world->height) {
                double dist_sq = (rx > 0.001 ? (dx / rx) * (dx / rx) : 0.0) +
                    (ry > 0.001 ? (dy / ry) * (dy / ry) : 0.0);
                if (dist_sq <= 1.0) {
                    double a = GetTextAlphaBilinear(world, n_x, n_y, is_deep);
                    if (a > max_alpha) {
                        max_alpha = a;
                        if (max_alpha >= 0.999) {
                            return 1.0; // Early return since we found maximum possible alpha
                        }
                    }
                }
            }
        }
    }
    return max_alpha;
}

inline void GetBoxCoverage(double x, double y, const RenderRefcon* refcon,
    const SampleData& sample, double* out_r,
    double* out_g, double* out_b, double* out_a,
    bool is_deep) {
    double max_accum_alpha = 0.0;
    double blended_r = 0.0;
    double blended_g = 0.0;
    double blended_b = 0.0;

    const std::vector<size_t>* row_candidates = nullptr;
    if (!refcon->repeater_enable && !sample.row_box_candidates.empty()) {
        const int row = (int)floor(y);
        if (row < 0 || row >= (int)sample.row_box_candidates.size()) {
            *out_r = *out_g = *out_b = *out_a = 0.0;
            return;
        }
        row_candidates = &sample.row_box_candidates[row];
    }

    const size_t candidate_count =
        row_candidates ? row_candidates->size() : sample.boxes.size();
    for (size_t ci = 0; ci < candidate_count; ++ci) {
        const size_t bi = row_candidates ? (*row_candidates)[ci] : ci;
        const BBox& box = sample.boxes[bi];
        const PrecomputedBox& pb = sample.precomputed[bi];

        double box_cx = pb.box_cx;
        double box_cy = pb.box_cy;
        double box_w = pb.box_w;
        double box_h = pb.box_h;

        if (box_w <= 0.0 || box_h <= 0.0)
            continue;

        int start_col = refcon->repeater_enable ? -refcon->repeater_count_l : 0;
        int end_col = refcon->repeater_enable ? refcon->repeater_count_r : 0;
        int start_row = refcon->repeater_enable ? -refcon->repeater_count_u : 0;
        int end_row = refcon->repeater_enable ? refcon->repeater_count_d : 0;

        for (int row = start_row; row <= end_row; ++row) {
            for (int col = start_col; col <= end_col; ++col) {
                // Repeated center coordinates
                double box_cx_rep = pb.box_cx + col * (pb.box_w + refcon->repeater_gap_x) + row * refcon->repeater_offset_x;
                double box_cy_rep = pb.box_cy + row * (pb.box_h + refcon->repeater_gap_y) + col * refcon->repeater_offset_y;

                double dx = x - box_cx_rep;
                double dy = y - box_cy_rep;

                // Tight rotated bounding-box check
                double half_w = pb.half_w;
                double half_h = pb.half_h;
                double clamped_r = pb.clamped_r;
                double bound_x = pb.bound_x;
                double bound_y = pb.bound_y;

                if (abs(dx) > bound_x || abs(dy) > bound_y) {
                    continue;
                }

                // Rotate point
                double rx = dx * sample.cos_sample_theta - dy * sample.sin_sample_theta;
                double ry = dx * sample.sin_sample_theta + dy * sample.cos_sample_theta;

                double px_local = abs(rx);
                double py_local = abs(ry);

                double qx = px_local - (half_w - clamped_r);
                double qy = py_local - (half_h - clamped_r);

                double val_x = qx > 0.0 ? qx : 0.0;
                double val_y = qy > 0.0 ? qy : 0.0;
                double corner_dist = sqrt(val_x * val_x + val_y * val_y);

                double max_q = qx > qy ? qx : qy;
                double inside_dist = max_q < 0.0 ? max_q : 0.0;

                double dist = corner_dist + inside_dist - clamped_r;

                // If repeater is enabled, we use show_text / show_shape.
                // If repeater is disabled, we do not draw text inside GetBoxCoverage
                // for box modes, because the original text is already drawn at its
                // unshifted position (or not drawn at all in Boxes Only mode).
                // We only draw it for text modes where the text itself is the shape.
                bool show_text = true;
                if (refcon->repeater_enable) {
                    show_text = refcon->repeater_show_text;
                }
                else {
                    bool is_text_mode = (refcon->mode == MGA_MODE_TEXT_OUTLINE ||
                        refcon->mode == MGA_MODE_TEXT_SOLID ||
                        refcon->mode == MGA_MODE_TEXT_BOTH);
                    show_text = is_text_mode;
                }
                bool show_shape = refcon->repeater_enable ? refcon->repeater_show_shape : true;

                if (!show_text && !show_shape) {
                    continue;
                }

                double shape_a = 0.0;
                double shape_r = 0.0, shape_g = 0.0, shape_b = 0.0;

                // Calculate Shape Box Coverage
                if (show_shape) {
                    if (refcon->mode == MGA_MODE_CUSTOM_LAYER && sample.has_custom_layer) {
                        double bounds_w = sample.custom_bounds.max_x - sample.custom_bounds.min_x + 1.0;
                        double bounds_h = sample.custom_bounds.max_y - sample.custom_bounds.min_y + 1.0;
                        if (bounds_w < 1.0) bounds_w = 1.0;
                        if (bounds_h < 1.0) bounds_h = 1.0;

                        double aspect_custom = bounds_w / bounds_h;
                        double sprite_w = box_w;
                        double sprite_h = box_h;

                        if (refcon->keep_aspect) {
                            double aspect_box = box_w / box_h;
                            if (aspect_custom > aspect_box) {
                                sprite_w = box_w;
                                sprite_h = box_w / aspect_custom;
                            }
                            else {
                                sprite_h = box_h;
                                sprite_w = box_h * aspect_custom;
                            }
                        }

                        double u_content = (rx / sprite_w) + 0.5;
                        double v_content = (ry / sprite_h) + 0.5;

                        if (u_content >= 0.0 && u_content <= 1.0 && v_content >= 0.0 && v_content <= 1.0) {
                            double pixel_x = sample.custom_bounds.min_x + u_content * (bounds_w - 1.0);
                            double pixel_y = sample.custom_bounds.min_y + v_content * (bounds_h - 1.0);

                            double smp_r = 0.0, smp_g = 0.0, smp_b = 0.0, smp_a = 0.0;
                            double max_val = is_deep ? 32768.0 : 255.0;
                            if (is_deep) {
                                sample_bilinear_xy<PF_Pixel16, PF_Pixel16>(&sample.custom_world, pixel_x, pixel_y, &smp_r, &smp_g, &smp_b, &smp_a, max_val);
                            }
                            else {
                                sample_bilinear_xy<PF_Pixel8, PF_Pixel8>(&sample.custom_world, pixel_x, pixel_y, &smp_r, &smp_g, &smp_b, &smp_a, max_val);
                            }

                            double mask_a = (dist < 0.5) ? (dist < -0.5 ? 1.0 : (0.5 - dist)) : 0.0;
                            smp_a *= mask_a;

                            shape_a = smp_a * (refcon->box_opacity / 100.0) * box.max_alpha;
                            shape_r = smp_r;
                            shape_g = smp_g;
                            shape_b = smp_b;
                        }
                    }
                    else {
                        double solid_a = 0.0;
                        if (refcon->mode == MGA_MODE_SOLID || refcon->mode == MGA_MODE_BOTH) {
                            solid_a = (dist < 0.5) ? (dist < -0.5 ? 1.0 : (0.5 - dist)) : 0.0;
                            solid_a *= (refcon->box_opacity / 100.0) * box.max_alpha;
                        }

                        double outline_a = 0.0;
                        if (refcon->mode == MGA_MODE_OUTLINE || refcon->mode == MGA_MODE_BOTH) {
                            double outline_dist = abs(dist) - (refcon->render_stroke_width / 2.0);
                            outline_a = (outline_dist < 0.5) ? (outline_dist < -0.5 ? 1.0 : (0.5 - outline_dist)) : 0.0;
                            outline_a *= (refcon->outline_opacity / 100.0) * box.max_alpha;
                        }

                        if (refcon->mode == MGA_MODE_BOTH) {
                            shape_a = outline_a + solid_a * (1.0 - outline_a);
                            if (shape_a > 0.0) {
                                double outline_r = refcon->outline_color.red / 255.0;
                                double outline_g = refcon->outline_color.green / 255.0;
                                double outline_b = refcon->outline_color.blue / 255.0;

                                double solid_r = refcon->box_color.red / 255.0;
                                double solid_g = refcon->box_color.green / 255.0;
                                double solid_b = refcon->box_color.blue / 255.0;

                                shape_r = (outline_r * outline_a + solid_r * solid_a * (1.0 - outline_a)) / shape_a;
                                shape_g = (outline_g * outline_a + solid_g * solid_a * (1.0 - outline_a)) / shape_a;
                                shape_b = (outline_b * outline_a + solid_b * solid_a * (1.0 - outline_a)) / shape_a;
                            }
                        }
                        else if (refcon->mode == MGA_MODE_OUTLINE) {
                            shape_a = outline_a;
                            shape_r = refcon->outline_color.red / 255.0;
                            shape_g = refcon->outline_color.green / 255.0;
                            shape_b = refcon->outline_color.blue / 255.0;
                        }
                        else {
                            shape_a = solid_a;
                            shape_r = refcon->box_color.red / 255.0;
                            shape_g = refcon->box_color.green / 255.0;
                            shape_b = refcon->box_color.blue / 255.0;
                        }
                    }
                }

                double text_a = 0.0;
                double text_r = 0.0, text_g = 0.0, text_b = 0.0;

                // Calculate Text Coverage
                if (show_text) {
                    double cx_geom = box_cx - refcon->render_offset_x;
                    double cy_geom = box_cy - refcon->render_offset_y;

                    double dx_local = rx / (refcon->scale_x_pct / 100.0);
                    double dy_local = ry / (refcon->scale_y_pct / 100.0);
                    double ux = dx_local;
                    double uy = dy_local;

                    double ox = cx_geom + ux;
                    double oy = cy_geom + uy;



                    double rx_dil = 0.0;
                    double ry_dil = 0.0;
                    if (refcon->mode == MGA_MODE_TEXT_OUTLINE || refcon->mode == MGA_MODE_TEXT_BOTH) {
                        rx_dil = refcon->render_stroke_width / (refcon->scale_x_pct / 100.0);
                        ry_dil = refcon->render_stroke_width / (refcon->scale_y_pct / 100.0);
                    }

                    if (ox >= box.min_x - rx_dil - 4.0 && ox <= box.max_x + rx_dil + 4.0 &&
                        oy >= box.min_y - ry_dil - 4.0 && oy <= box.max_y + ry_dil + 4.0) {

                        double orig_r = 0.0, orig_g = 0.0, orig_b = 0.0, orig_a = 0.0;
                        double max_val = is_deep ? 32768.0 : 255.0;

                        // Bounds check: if ox/oy is outside the input world buffer,
                        // return transparent instead of clamping to edge pixels.
                        // This prevents text corruption when the layer is moved
                        // partially off-screen or during repeater offset calculations.
                        if (ox >= 0.0 && ox < sample.input_world.width &&
                            oy >= 0.0 && oy < sample.input_world.height) {
                            if (is_deep) {
                                sample_bilinear_xy<PF_Pixel16, PF_Pixel16>(&sample.input_world, ox, oy, &orig_r, &orig_g, &orig_b, &orig_a, max_val);
                            }
                            else {
                                sample_bilinear_xy<PF_Pixel8, PF_Pixel8>(&sample.input_world, ox, oy, &orig_r, &orig_g, &orig_b, &orig_a, max_val);
                            }
                        }

                        if (refcon->mode == MGA_MODE_TEXT_OUTLINE ||
                            refcon->mode == MGA_MODE_TEXT_SOLID ||
                            refcon->mode == MGA_MODE_TEXT_BOTH) {

                            double fill_a = GetMaxAlphaInNeighborhood(ox, oy, rx_dil, ry_dil, &sample.input_world, is_deep);
                            double solid_a = 0.0;
                            if (refcon->mode == MGA_MODE_TEXT_SOLID || refcon->mode == MGA_MODE_TEXT_BOTH) {
                                solid_a = orig_a * (refcon->box_opacity / 100.0);
                            }

                            double outline_a = 0.0;
                            if (refcon->mode == MGA_MODE_TEXT_OUTLINE || refcon->mode == MGA_MODE_TEXT_BOTH) {
                                double diff = fill_a - orig_a;
                                if (diff < 0.0) diff = 0.0;
                                outline_a = diff * (refcon->outline_opacity / 100.0);
                            }

                            if (refcon->mode == MGA_MODE_TEXT_BOTH) {
                                text_a = outline_a + solid_a * (1.0 - outline_a);
                                if (text_a > 0.0) {
                                    double outline_r = refcon->outline_color.red / 255.0;
                                    double outline_g = refcon->outline_color.green / 255.0;
                                    double outline_b = refcon->outline_color.blue / 255.0;

                                    double solid_r = refcon->box_color.red / 255.0;
                                    double solid_g = refcon->box_color.green / 255.0;
                                    double solid_b = refcon->box_color.blue / 255.0;

                                    text_r = (outline_r * outline_a + solid_r * solid_a * (1.0 - outline_a)) / text_a;
                                    text_g = (outline_g * outline_a + solid_g * solid_a * (1.0 - outline_a)) / text_a;
                                    text_b = (outline_b * outline_a + solid_b * solid_a * (1.0 - outline_a)) / text_a;
                                }
                            }
                            else if (refcon->mode == MGA_MODE_TEXT_OUTLINE) {
                                if (refcon->fill_outline) {
                                    text_a = outline_a + orig_a * (1.0 - outline_a);
                                    if (text_a > 0.0) {
                                        double outline_r = refcon->outline_color.red / 255.0;
                                        double outline_g = refcon->outline_color.green / 255.0;
                                        double outline_b = refcon->outline_color.blue / 255.0;

                                        text_r = (outline_r * outline_a + orig_r * orig_a * (1.0 - outline_a)) / text_a;
                                        text_g = (outline_g * outline_a + orig_g * orig_a * (1.0 - outline_a)) / text_a;
                                        text_b = (outline_b * outline_a + orig_b * orig_a * (1.0 - outline_a)) / text_a;
                                    }
                                }
                                else {
                                    text_a = outline_a;
                                    text_r = refcon->outline_color.red / 255.0;
                                    text_g = refcon->outline_color.green / 255.0;
                                    text_b = refcon->outline_color.blue / 255.0;
                                }
                            }
                            else {
                                text_a = solid_a;
                                text_r = refcon->box_color.red / 255.0;
                                text_g = refcon->box_color.green / 255.0;
                                text_b = refcon->box_color.blue / 255.0;
                            }
                        }
                        else {
                            // For box modes (Outline, Solid Box, Outline & Solid, Custom Layer),
                            // if Show Text is enabled, draw the original text over the box
                            text_a = orig_a;
                            text_r = orig_r;
                            text_g = orig_g;
                            text_b = orig_b;
                        }
                    }
                }

                // Composite/blend text on top of shape box
                double current_alpha = text_a + shape_a * (1.0 - text_a);
                double cur_r = 0.0, cur_g = 0.0, cur_b = 0.0;
                if (current_alpha > 0.0) {
                    cur_r = (text_r * text_a + shape_r * shape_a * (1.0 - text_a)) / current_alpha;
                    cur_g = (text_g * text_a + shape_g * shape_a * (1.0 - text_a)) / current_alpha;
                    cur_b = (text_b * text_a + shape_b * shape_a * (1.0 - text_a)) / current_alpha;
                }

                // Accumulate coverage across all boxes and repeats using standard alpha blending
                if (current_alpha > 0.0) {
                    if (max_accum_alpha == 0.0) {
                        max_accum_alpha = current_alpha;
                        blended_r = cur_r;
                        blended_g = cur_g;
                        blended_b = cur_b;
                    }
                    else {
                        double old_a = max_accum_alpha;
                        max_accum_alpha = current_alpha + old_a * (1.0 - current_alpha);
                        if (max_accum_alpha > 0.0) {
                            blended_r = (cur_r * current_alpha + blended_r * old_a * (1.0 - current_alpha)) / max_accum_alpha;
                            blended_g = (cur_g * current_alpha + blended_g * old_a * (1.0 - current_alpha)) / max_accum_alpha;
                            blended_b = (cur_b * current_alpha + blended_b * old_a * (1.0 - current_alpha)) / max_accum_alpha;
                        }
                    }
                }
            }
        }
    }

    *out_r = blended_r;
    *out_g = blended_g;
    *out_b = blended_b;
    *out_a = max_accum_alpha;
}

inline void RefineSubpixelBBoxes(std::vector<BBox>& boxes,
    PF_EffectWorld* input, bool is_deep) {
    if (!input || !input->data || boxes.empty()) {
        return;
    }

    double max_val = is_deep ? 32768.0 : 255.0;

    for (auto& box : boxes) {
        // Skip invalid bounding boxes
        if (box.min_x > box.max_x || box.min_y > box.max_y) {
            continue;
        }

        // 1. Calculate subpixel centroid (cx, cy)
        double sum_alpha = 0.0;
        double sum_x = 0.0;
        double sum_y = 0.0;

        // Expand search range by 2 pixels on all sides to prevent the active pixel scanning
        // from being truncated when the integer CCL bounding box jumps.
        int int_min_x = (int)floor(box.min_x) - 2;
        int int_max_x = (int)ceil(box.max_x) + 2;
        int int_min_y = (int)floor(box.min_y) - 2;
        int int_max_y = (int)ceil(box.max_y) + 2;

        if (int_min_x < 0) int_min_x = 0;
        if (int_max_x >= input->width) int_max_x = input->width - 1;
        if (int_min_y < 0) int_min_y = 0;
        if (int_max_y >= input->height) int_max_y = input->height - 1;

        int width_range = int_max_x - int_min_x + 1;
        int height_range = int_max_y - int_min_y + 1;
        if (width_range <= 0 || height_range <= 0) {
            continue;
        }

        std::vector<double> ProfileX(width_range, 0.0);
        std::vector<double> ProfileY(height_range, 0.0);

        for (int y = int_min_y; y <= int_max_y; ++y) {
            void* row = (char*)input->data + y * input->rowbytes;
            for (int x = int_min_x; x <= int_max_x; ++x) {
                double alpha = is_deep ? (((PF_Pixel16*)row)[x].alpha / max_val)
                    : (((PF_Pixel8*)row)[x].alpha / max_val);
                if (alpha > 0.0) {
                    sum_alpha += alpha;
                    sum_x += x * alpha;
                    sum_y += y * alpha;
                    ProfileX[x - int_min_x] += alpha;
                    ProfileY[y - int_min_y] += alpha;
                }
            }
        }

        if (sum_alpha > 0.0) {
            box.cx = sum_x / sum_alpha;
            box.cy = sum_y / sum_alpha;
        }
        else {
            box.cx = (box.min_x + box.max_x) / 2.0;
            box.cy = (box.min_y + box.max_y) / 2.0;
        }

        double orig_min_x = box.min_x;
        double orig_max_x = box.max_x;
        double orig_min_y = box.min_y;
        double orig_max_y = box.max_y;

        double sum_alpha_x = 0.0;
        for (double val : ProfileX) sum_alpha_x += val;
        double sum_alpha_y = 0.0;
        for (double val : ProfileY) sum_alpha_y += val;

        // Cumulative sum threshold (5.0 alpha represents about 5 pixels of opaque height,
        // which lies in the linear region of the cumulative sum and has zero interpolation error).
        double T_x = 5.0;
        double T_y = 5.0;

        double max_px = 0.0;
        for (double val : ProfileX) { if (val > max_px) max_px = val; }
        double max_py = 0.0;
        for (double val : ProfileY) { if (val > max_py) max_py = val; }

        // Adapt threshold for small components
        if (sum_alpha_x < 50.0) {
            T_x = 0.1 * sum_alpha_x;
        }
        if (sum_alpha_y < 50.0) {
            T_y = 0.1 * sum_alpha_y;
        }

        double offset_x = T_x / (max_px > 0.0 ? max_px : 1.0) + 0.5;
        double offset_y = T_y / (max_py > 0.0 ? max_py : 1.0) + 0.5;

        // Limit offset to reasonable values to prevent extreme cases
        if (offset_x > 3.0) offset_x = 3.0;
        if (offset_y > 3.0) offset_y = 3.0;

        // 2. Refine min_x and max_x using cumulative sum
        if (sum_alpha_x > 2.0 * T_x) {
            // Left boundary (cumulative from left to right)
            double cum = 0.0;
            for (size_t i = 0; i < ProfileX.size(); ++i) {
                double val = ProfileX[i];
                double prev_cum = cum;
                cum += val;
                if (cum > T_x) {
                    double interp_i = (double)i;
                    if (val > 0.0) {
                        interp_i = (double)i - 1.0 + (T_x - prev_cum) / val;
                    }
                    box.min_x = int_min_x + interp_i - offset_x;
                    break;
                }
            }

            // Right boundary (cumulative from right to left)
            cum = 0.0;
            for (int i = (int)ProfileX.size() - 1; i >= 0; --i) {
                double val = ProfileX[i];
                double prev_cum = cum;
                cum += val;
                if (cum > T_x) {
                    double interp_i = (double)i;
                    if (val > 0.0) {
                        interp_i = (double)i + 1.0 - (T_x - prev_cum) / val;
                    }
                    box.max_x = int_min_x + interp_i + offset_x;
                    break;
                }
            }
        }

        // 3. Refine min_y and max_y using cumulative sum
        if (sum_alpha_y > 2.0 * T_y) {
            // Top boundary (cumulative from top to bottom)
            double cum = 0.0;
            for (size_t i = 0; i < ProfileY.size(); ++i) {
                double val = ProfileY[i];
                double prev_cum = cum;
                cum += val;
                if (cum > T_y) {
                    double interp_i = (double)i;
                    if (val > 0.0) {
                        interp_i = (double)i - 1.0 + (T_y - prev_cum) / val;
                    }
                    box.min_y = int_min_y + interp_i - offset_y;
                    break;
                }
            }

            // Bottom boundary (cumulative from bottom to top)
            cum = 0.0;
            for (int i = (int)ProfileY.size() - 1; i >= 0; --i) {
                double val = ProfileY[i];
                double prev_cum = cum;
                cum += val;
                if (cum > T_y) {
                    double interp_i = (double)i;
                    if (val > 0.0) {
                        interp_i = (double)i + 1.0 - (T_y - prev_cum) / val;
                    }
                    box.max_y = int_min_y + interp_i + offset_y;
                    break;
                }
            }
        }

        double var_x = 0.0;
        if (sum_alpha > 0.0) {
            double sum_sq_diff = 0.0;
            for (size_t i = 0; i < ProfileX.size(); ++i) {
                double x_coord = (double)int_min_x + (double)i;
                sum_sq_diff += ProfileX[i] * (x_coord - box.cx) * (x_coord - box.cx);
            }
            var_x = sum_sq_diff / sum_alpha;
        }
        double sigma_x = sqrt(var_x);

        {
            std::stringstream ss;
            ss << "Refinement: "
                << "Input=[" << orig_min_x << ", " << orig_max_x << ", " << orig_min_y << ", " << orig_max_y << "] "
                << "Output=[" << box.min_x << ", " << box.max_x << ", " << box.min_y << ", " << box.max_y << "] "
                << "W=" << (box.max_x - box.min_x + 1.0) << " H=" << (box.max_y - box.min_y + 1.0) << " "
                << "GeomCenter=[" << (box.min_x + box.max_x) / 2.0 << ", " << (box.min_y + box.max_y) / 2.0 << "] "
                << "sigma_x=" << sigma_x << " ratio=" << ((box.max_x - box.min_x) / (sigma_x > 0.0 ? sigma_x : 1.0));
            MGA_LOG(ss.str());
        }

        {
            std::stringstream ss;
            ss << "ProfileX right edge values: ";
            int start_idx = max(0, (int)ProfileX.size() - 8);
            for (int i = start_idx; i < (int)ProfileX.size(); ++i) {
                ss << "col[" << (int_min_x + i) << "]=" << ProfileX[i] << " ";
            }
            MGA_LOG(ss.str());
        }
    }
}

static void UnrotateChunk(
    int start_y, int end_y, int width, int height,
    const PF_EffectWorld* input, PF_EffectWorld* output,
    double cos_t, double sin_t, double cx, double cy, bool is_deep) {
    for (int y = start_y; y < end_y; ++y) {
        if (is_deep) {
            PF_Pixel16* row = (PF_Pixel16*)((char*)output->data + y * output->rowbytes);
            for (int x = 0; x < width; ++x) {
                double dx = (double)x - cx;
                double dy = (double)y - cy;
                double rx = cx + dx * cos_t - dy * sin_t;
                double ry = cy + dx * sin_t + dy * cos_t;

                if (rx >= 0.0 && rx < width && ry >= 0.0 && ry < height) {
                    double r = 0.0, g = 0.0, b = 0.0, a = 0.0;
                    sample_bilinear_xy<PF_Pixel16, PF_Pixel16>(input, rx, ry, &r, &g, &b, &a, 32768.0);
                    row[x].alpha = (A_u_short)clamped_channel(a * 32768.0, 32768.0);
                    row[x].red = (A_u_short)clamped_channel(r * 32768.0, 32768.0);
                    row[x].green = (A_u_short)clamped_channel(g * 32768.0, 32768.0);
                    row[x].blue = (A_u_short)clamped_channel(b * 32768.0, 32768.0);
                }
            }
        }
        else {
            PF_Pixel8* row = (PF_Pixel8*)((char*)output->data + y * output->rowbytes);
            for (int x = 0; x < width; ++x) {
                double dx = (double)x - cx;
                double dy = (double)y - cy;
                double rx = cx + dx * cos_t - dy * sin_t;
                double ry = cy + dx * sin_t + dy * cos_t;

                if (rx >= 0.0 && rx < width && ry >= 0.0 && ry < height) {
                    double r = 0.0, g = 0.0, b = 0.0, a = 0.0;
                    sample_bilinear_xy<PF_Pixel8, PF_Pixel8>(input, rx, ry, &r, &g, &b, &a, 255.0);
                    row[x].alpha = (A_u_char)clamped_channel(a * 255.0, 255.0);
                    row[x].red = (A_u_char)clamped_channel(r * 255.0, 255.0);
                    row[x].green = (A_u_char)clamped_channel(g * 255.0, 255.0);
                    row[x].blue = (A_u_char)clamped_channel(b * 255.0, 255.0);
                }
            }
        }
    }
}

static PF_Err UnrotateInputWorld(PF_InData* in_data, const PF_EffectWorld* input, double theta, double cx, double cy, PF_EffectWorld* output) {
    PF_Err err = PF_Err_NONE;
    int width = input->width;
    int height = input->height;
    bool is_deep = PF_WORLD_IS_DEEP(input);

    const size_t pixel_size = is_deep ? sizeof(PF_Pixel16) : sizeof(PF_Pixel8);
    for (int y = 0; y < height; ++y) {
        void* dst_row = (char*)output->data + y * output->rowbytes;
        memset(dst_row, 0, (size_t)width * pixel_size);
    }

    double cos_t = cos(theta);
    double sin_t = sin(theta);

    unsigned int num_threads = std::thread::hardware_concurrency();
    if (num_threads == 0) num_threads = 4;
    num_threads = min(num_threads, 16u);

    int chunk_size = (height + (int)num_threads - 1) / (int)num_threads;
    if (chunk_size == 0) chunk_size = 1;

    std::vector<std::thread> threads;
    threads.reserve(num_threads);

    for (unsigned int i = 0; i < num_threads; ++i) {
        int start_y = (int)i * chunk_size;
        int end_y = min(height, start_y + chunk_size);
        if (start_y < end_y) {
            threads.emplace_back(UnrotateChunk, start_y, end_y, width, height, input, output, cos_t, sin_t, cx, cy, is_deep);
        }
    }

    for (auto& t : threads) {
        if (t.joinable()) {
            t.join();
        }
    }

    return err;
}

std::vector<BBox> FindTargetBoxes(PF_InData* in_data, PF_EffectWorld* input,
    int target_mode, double word_gap_mult,
    double line_gap_mult) {
    std::vector<BBox> result;
    if (!input || !input->data) {
        return result;
    }
    int width = input->width;
    int height = input->height;
    bool is_deep = PF_WORLD_IS_DEEP(input);

    // Restrict alpha discovery to AE's conservative non-transparent extent.
    // Empty/invalid hints fall back to the complete input world.
    int scan_min_x = 0, scan_max_x = width - 1;
    int scan_min_y = 0, scan_max_y = height - 1;
    const PF_Rect& hint = input->extent_hint;
    if (hint.right > hint.left && hint.bottom > hint.top) {
        scan_min_x = max(0, (int)hint.left - 2);
        scan_max_x = min(width - 1, (int)hint.right + 1);
        scan_min_y = max(0, (int)hint.top - 2);
        scan_max_y = min(height - 1, (int)hint.bottom + 1);
    }

    int g_min_x = width, g_max_x = -1;
    int g_min_y = height, g_max_y = -1;

    int active_pixel_count = 0;
    double global_max_alpha = 0.0;

    if (is_deep) {
        for (int y = scan_min_y; y <= scan_max_y; ++y) {
            PF_Pixel16* row =
                (PF_Pixel16*)((char*)input->data + y * input->rowbytes);
            for (int x = scan_min_x; x <= scan_max_x; ++x) {
                if (row[x].alpha > 640) {
                    active_pixel_count++;
                    const double alpha_val = row[x].alpha / 32768.0;
                    if (alpha_val > global_max_alpha) global_max_alpha = alpha_val;
                    if (x < g_min_x)
                        g_min_x = x;
                    if (x > g_max_x)
                        g_max_x = x;
                    if (y < g_min_y)
                        g_min_y = y;
                    if (y > g_max_y)
                        g_max_y = y;
                }
            }
        }
    }
    else {
        for (int y = scan_min_y; y <= scan_max_y; ++y) {
            PF_Pixel8* row = (PF_Pixel8*)((char*)input->data + y * input->rowbytes);
            for (int x = scan_min_x; x <= scan_max_x; ++x) {
                if (row[x].alpha > 5) {
                    active_pixel_count++;
                    const double alpha_val = row[x].alpha / 255.0;
                    if (alpha_val > global_max_alpha) global_max_alpha = alpha_val;
                    if (x < g_min_x)
                        g_min_x = x;
                    if (x > g_max_x)
                        g_max_x = x;
                    if (y < g_min_y)
                        g_min_y = y;
                    if (y > g_max_y)
                        g_max_y = y;
                }
            }
        }
    }

    if (g_max_x < g_min_x || g_max_y < g_min_y) {
        return result;
    }

    // Whole Text only needs the global alpha bounds. The old path still ran
    // connected-component labelling, union-find, merging and line grouping,
    // only to collapse everything back to this same rectangle.
    if (target_mode == MGA_TARGET_WHOLE) {
        BBox whole = { (double)g_min_x, (double)g_max_x,
                      (double)g_min_y, (double)g_max_y,
                      0.0, 0.0, global_max_alpha };
        result.push_back(whole);
        RefineSubpixelBBoxes(result, input, is_deep);
        return result;
    }

    int w_tb = g_max_x - g_min_x + 1;
    int h_tb = g_max_y - g_min_y + 1;

    // Per Character used to assume horizontal typography and split only on
    // empty columns. For a vertical text layer that turns the full column into
    // one tall character box, so rotating the effect produces one long slab.
    // Detect a clearly vertical layout and segment it by occupied row runs.
    if (target_mode == MGA_TARGET_CHAR && h_tb > w_tb * 3 / 2) {
        std::vector<bool> row_active(h_tb, false);
        for (int y = 0; y < h_tb; ++y) {
            const int img_y = g_min_y + y;
            void* row_ptr = (char*)input->data + img_y * input->rowbytes;
            for (int x = g_min_x; x <= g_max_x; ++x) {
                const bool active = is_deep
                    ? (((PF_Pixel16*)row_ptr)[x].alpha > 640)
                    : (((PF_Pixel8*)row_ptr)[x].alpha > 5);
                if (active) {
                    row_active[y] = true;
                    break;
                }
            }
        }

        std::vector<std::pair<int, int>> raw_runs;
        int run_start = -1;
        for (int y = 0; y < h_tb; ++y) {
            if (row_active[y]) {
                if (run_start < 0) run_start = y;
            }
            else if (run_start >= 0) {
                raw_runs.push_back(std::make_pair(run_start, y - 1));
                run_start = -1;
            }
        }
        if (run_start >= 0) {
            raw_runs.push_back(std::make_pair(run_start, h_tb - 1));
        }

        // Rejoin only tiny internal gaps (for dots/accents and disconnected
        // Japanese strokes). Real character spacing remains separated.
        const int internal_gap = max(1, (int)round(w_tb * 0.06));
        std::vector<std::pair<int, int>> char_runs;
        for (size_t i = 0; i < raw_runs.size(); ++i) {
            if (char_runs.empty() ||
                raw_runs[i].first - char_runs.back().second - 1 > internal_gap) {
                char_runs.push_back(raw_runs[i]);
            }
            else {
                char_runs.back().second = raw_runs[i].second;
            }
        }

        std::vector<BBox> vertical_boxes;
        for (size_t ri = 0; ri < char_runs.size(); ++ri) {
            const int min_y = g_min_y + char_runs[ri].first;
            const int max_y = g_min_y + char_runs[ri].second;
            int min_x = width;
            int max_x = -1;
            double max_alpha = 0.0;

            for (int y = min_y; y <= max_y; ++y) {
                void* row_ptr = (char*)input->data + y * input->rowbytes;
                for (int x = g_min_x; x <= g_max_x; ++x) {
                    double alpha = 0.0;
                    if (is_deep) {
                        alpha = ((PF_Pixel16*)row_ptr)[x].alpha / 32768.0;
                    }
                    else {
                        alpha = ((PF_Pixel8*)row_ptr)[x].alpha / 255.0;
                    }
                    if (alpha > (is_deep ? 640.0 / 32768.0 : 5.0 / 255.0)) {
                        if (x < min_x) min_x = x;
                        if (x > max_x) max_x = x;
                        if (alpha > max_alpha) max_alpha = alpha;
                    }
                }
            }

            if (max_x >= min_x) {
                BBox character = { (double)min_x, (double)max_x,
                                  (double)min_y, (double)max_y,
                                  0.0, 0.0, max_alpha };
                vertical_boxes.push_back(character);
            }
        }

        // A single run means the row projection could not prove character
        // boundaries; retain the component-based fallback below in that case.
        if (vertical_boxes.size() > 1) {
            std::sort(vertical_boxes.begin(), vertical_boxes.end(),
                [](const BBox& a, const BBox& b) {
                    return a.min_y < b.min_y;
                });
            RefineSubpixelBBoxes(vertical_boxes, input, is_deep);
            return vertical_boxes;
        }
    }

    std::vector<int> labels(w_tb * h_tb, 0);
    UnionFind uf(w_tb * h_tb + 10);
    int next_label = 1;

    for (int y = 0; y < h_tb; ++y) {
        int img_y = g_min_y + y;
        void* row_ptr = (char*)input->data + img_y * input->rowbytes;

        for (int x = 0; x < w_tb; ++x) {
            int img_x = g_min_x + x;
            bool active = false;
            if (is_deep) {
                active = (((PF_Pixel16*)row_ptr)[img_x].alpha > 640);
            }
            else {
                active = (((PF_Pixel8*)row_ptr)[img_x].alpha > 5);
            }

            if (active) {
                int neighbors[4];
                int neighbor_count = 0;
                if (x > 0 && labels[y * w_tb + x - 1] > 0)
                    neighbors[neighbor_count++] = labels[y * w_tb + x - 1];
                if (y > 0 && labels[(y - 1) * w_tb + x] > 0)
                    neighbors[neighbor_count++] = labels[(y - 1) * w_tb + x];
                if (target_mode != MGA_TARGET_CHAR) {
                    if (x > 0 && y > 0 && labels[(y - 1) * w_tb + x - 1] > 0)
                        neighbors[neighbor_count++] = labels[(y - 1) * w_tb + x - 1];
                    if (x < w_tb - 1 && y > 0 && labels[(y - 1) * w_tb + x + 1] > 0)
                        neighbors[neighbor_count++] = labels[(y - 1) * w_tb + x + 1];
                }

                if (neighbor_count == 0) {
                    labels[y * w_tb + x] = next_label;
                    next_label++;
                }
                else {
                    int min_l = neighbors[0];
                    for (int ni = 0; ni < neighbor_count; ++ni) {
                        const int n = neighbors[ni];
                        if (n < min_l)
                            min_l = n;
                    }
                    labels[y * w_tb + x] = min_l;
                    for (int ni = 0; ni < neighbor_count; ++ni) {
                        uf.unite(min_l, neighbors[ni]);
                    }
                }
            }
        }
    }

    std::map<int, BBox> label_bboxes;
    for (int y = 0; y < h_tb; ++y) {
        int img_y = g_min_y + y;
        void* row_ptr = (char*)input->data + img_y * input->rowbytes;
        for (int x = 0; x < w_tb; ++x) {
            int l = labels[y * w_tb + x];
            if (l > 0) {
                int root_l = uf.find(l);
                int px = g_min_x + x;
                int py = g_min_y + y;

                double alpha_val = 0.0;
                if (is_deep) {
                    alpha_val = ((PF_Pixel16*)row_ptr)[px].alpha / 32768.0;
                }
                else {
                    alpha_val = ((PF_Pixel8*)row_ptr)[px].alpha / 255.0;
                }

                if (label_bboxes.find(root_l) == label_bboxes.end()) {
                    BBox box = { (double)px, (double)px, (double)py, (double)py, 0.0, 0.0, alpha_val };
                    label_bboxes[root_l] = box;
                }
                else {
                    BBox& box = label_bboxes[root_l];
                    if ((double)px < box.min_x)
                        box.min_x = (double)px;
                    if ((double)px > box.max_x)
                        box.max_x = (double)px;
                    if ((double)py < box.min_y)
                        box.min_y = (double)py;
                    if ((double)py > box.max_y)
                        box.max_y = (double)py;
                    if (alpha_val > box.max_alpha)
                        box.max_alpha = alpha_val;
                }
            }
        }
    }

    std::vector<BBox> boxes;
    for (const auto& pair : label_bboxes) {
        const BBox& box = pair.second;
        int bw = (int)round(box.max_x - box.min_x + 1.0);
        int bh = (int)round(box.max_y - box.min_y + 1.0);
        if (bw <= 1 && bh <= 1) {
            continue; // Ignore 1x1 noise components
        }
        if (bw <= 2 && bh <= 2 && box.max_alpha < 0.1) {
            continue; // Ignore tiny faint noise components
        }
        boxes.push_back(box);
    }

    if (boxes.empty())
        return result;

    // Stable sort by min_x first
    std::sort(boxes.begin(), boxes.end(),
        [](const BBox& a, const BBox& b) { return a.min_x < b.min_x; });

    // NOTE: this loose, distance-only merge (h_dist <= 3 px) is meant to glue
    // together scattered fragments of the SAME character (like the dot and
    // stem of "i", or a base letter and its accent) before grouping into
    // words/lines. It has no way to tell that apart from two DIFFERENT
    // characters that simply happen to be close together.
    //
    // For MGA_TARGET_CHAR this is actively harmful: when a Position/Tracking
    // text animator moves characters closer together, adjacent-but-distinct
    // characters cross the 3px threshold every frame and get merged into one
    // box, then separate again next frame -- producing the "box appears/
    // disappears" flicker. Per-Character mode already has a much more
    // precise re-merge step below (based on horizontal overlap, not just
    // distance) that correctly reassembles true multi-part characters, so we
    // skip this generic merge entirely for MGA_TARGET_CHAR and let that
    // dedicated logic handle it.
    if (target_mode != MGA_TARGET_CHAR) {
        bool merged_any = true;
        while (merged_any) {
            merged_any = false;
            for (size_t i = 0; i < boxes.size(); ++i) {
                for (size_t j = i + 1; j < boxes.size(); ++j) {
                    BBox& A = boxes[i];
                    BBox& B = boxes[j];

                    double ay_h = A.max_y - A.min_y + 1.0;
                    double by_h = B.max_y - B.min_y + 1.0;
                    double avg_h = (ay_h + by_h) / 2.0;

                    double h_dist = 0.0;
                    if (A.max_x < B.min_x)
                        h_dist = B.min_x - A.max_x;
                    else if (B.max_x < A.min_x)
                        h_dist = A.min_x - B.max_x;
                    else
                        h_dist = 0.0;

                    double v_dist = 0.0;
                    if (A.max_y < B.min_y)
                        v_dist = B.min_y - A.max_y;
                    else if (B.max_y < A.min_y)
                        v_dist = A.min_y - B.max_y;
                    else
                        v_dist = 0.0;

                    if (h_dist <= 3.0 && v_dist <= avg_h * 0.8) {
                        A.min_x = min(A.min_x, B.min_x);
                        A.max_x = max(A.max_x, B.max_x);
                        A.min_y = min(A.min_y, B.min_y);
                        A.max_y = max(A.max_y, B.max_y);
                        A.max_alpha = max(A.max_alpha, B.max_alpha);

                        boxes.erase(boxes.begin() + j);
                        merged_any = true;
                        break;
                    }
                }
                if (merged_any)
                    break;
            }
        }
    }

    if (target_mode == MGA_TARGET_CHAR) {
        // Column-gap splitting: scan each box for vertical gaps to separate
        // characters that got merged (common at low preview resolutions)
        std::vector<BBox> split_boxes;
        for (const auto& box : boxes) {
            int bw = (int)round(box.max_x - box.min_x + 1.0);
            if (bw <= 2) {
                split_boxes.push_back(box);
                continue;
            }

            // Build column occupancy: does this column have any active pixels?
            std::vector<bool> col_active(bw, false);
            for (int col = 0; col < bw; ++col) {
                int img_x = (int)floor(box.min_x) + col;
                if (img_x < 0 || img_x >= width) {
                    continue;
                }
                int start_y = (int)floor(box.min_y);
                int end_y = (int)ceil(box.max_y);
                if (start_y < 0) start_y = 0;
                if (end_y >= height) end_y = height - 1;

                for (int row_y = start_y; row_y <= end_y; ++row_y) {
                    void* row_ptr = (char*)input->data + row_y * input->rowbytes;
                    bool active = false;
                    if (is_deep) {
                        active = (((PF_Pixel16*)row_ptr)[img_x].alpha > 640);
                    }
                    else {
                        active = (((PF_Pixel8*)row_ptr)[img_x].alpha > 5);
                    }
                    if (active) {
                        col_active[col] = true;
                        break;
                    }
                }
            }

            // Find runs of active columns and split at gaps
            int seg_start = -1;
            for (int col = 0; col < bw; ++col) {
                if (col_active[col]) {
                    if (seg_start < 0)
                        seg_start = col;
                }
                else {
                    if (seg_start >= 0) {
                        BBox seg = { box.min_x + seg_start,
                                    box.min_x + col - 1,
                                    box.min_y,
                                    box.max_y,
                                    0.0,
                                    0.0,
                                    box.max_alpha };
                        split_boxes.push_back(seg);
                        seg_start = -1;
                    }
                }
            }
            if (seg_start >= 0) {
                BBox seg = {
                    box.min_x + seg_start, box.max_x, box.min_y, box.max_y, 0.0, 0.0,
                    box.max_alpha };
                split_boxes.push_back(seg);
            }
        }

        // Re-merge segments that are very close vertically (e.g. "i" dot + body)
        // using the original merge logic but only vertically
        bool re_merged = true;
        while (re_merged) {
            re_merged = false;
            for (size_t i = 0; i < split_boxes.size(); ++i) {
                for (size_t j = i + 1; j < split_boxes.size(); ++j) {
                    BBox& A = split_boxes[i];
                    BBox& B = split_boxes[j];

                    // Check if they overlap horizontally (same column range)
                    double overlap_x =
                        max(0.0, min(A.max_x, B.max_x) - max(A.min_x, B.min_x) + 1.0);
                    double min_w = min(A.max_x - A.min_x + 1.0, B.max_x - B.min_x + 1.0);
                    if (overlap_x > min_w * 0.3) {
                        // Vertically close? Merge them
                        double v_dist = 0.0;
                        if (A.max_y < B.min_y)
                            v_dist = B.min_y - A.max_y;
                        else if (B.max_y < A.min_y)
                            v_dist = A.min_y - B.max_y;

                        double avg_h = ((A.max_y - A.min_y + 1.0) + (B.max_y - B.min_y + 1.0)) / 2.0;
                        if (v_dist <= avg_h * 0.5) {
                            A.min_x = min(A.min_x, B.min_x);
                            A.max_x = max(A.max_x, B.max_x);
                            A.min_y = min(A.min_y, B.min_y);
                            A.max_y = max(A.max_y, B.max_y);
                            A.max_alpha = max(A.max_alpha, B.max_alpha);
                            split_boxes.erase(split_boxes.begin() + j);
                            re_merged = true;
                            break;
                        }
                    }
                }
                if (re_merged)
                    break;
            }
        }

        // Sort split_boxes left-to-right for consistent rendering order
        std::sort(split_boxes.begin(), split_boxes.end(),
            [](const BBox& a, const BBox& b) { return a.min_x < b.min_x; });

        RefineSubpixelBBoxes(split_boxes, input, is_deep);
        return split_boxes;
    }

    std::vector<std::vector<BBox>> lines;
    for (const auto& box : boxes) {
        bool found_line = false;
        for (auto& line : lines) {
            // Find the combined vertical span of all boxes in this line
            double line_min_y = line[0].min_y;
            double line_max_y = line[0].max_y;
            for (const auto& b : line) {
                if (b.min_y < line_min_y)
                    line_min_y = b.min_y;
                if (b.max_y > line_max_y)
                    line_max_y = b.max_y;
            }
            double line_h = line_max_y - line_min_y + 1.0;
            double box_h = box.max_y - box.min_y + 1.0;
            double overlap =
                max(0.0, min(line_max_y, box.max_y) - max(line_min_y, box.min_y) + 1.0);
            if (overlap > 0.3 * min(line_h, box_h)) {
                line.push_back(box);
                found_line = true;
                break;
            }
        }
        if (!found_line) {
            std::vector<BBox> new_line;
            new_line.push_back(box);
            lines.push_back(new_line);
        }
    }

    if (target_mode == MGA_TARGET_WORD) {
        for (auto& line : lines) {
            std::sort(line.begin(), line.end(),
                [](const BBox& a, const BBox& b) { return a.min_x < b.min_x; });

            if (line.empty())
                continue;

            double avg_h = 0.0;
            for (const auto& b : line) {
                avg_h += (b.max_y - b.min_y + 1.0);
            }
            avg_h /= line.size();

            std::vector<BBox> word;
            word.push_back(line[0]);

            for (size_t i = 1; i < line.size(); ++i) {
                double gap = line[i].min_x - line[i - 1].max_x;
                if (gap > word_gap_mult * avg_h) {
                    BBox w_box = { word[0].min_x,    word[0].max_x, word[0].min_y,
                                  word[0].max_y,    0.0,           0.0,
                                  word[0].max_alpha };
                    for (size_t k = 1; k < word.size(); ++k) {
                        w_box.min_x = min(w_box.min_x, word[k].min_x);
                        w_box.max_x = max(w_box.max_x, word[k].max_x);
                        w_box.min_y = min(w_box.min_y, word[k].min_y);
                        w_box.max_y = max(w_box.max_y, word[k].max_y);
                        w_box.max_alpha = max(w_box.max_alpha, word[k].max_alpha);
                    }
                    result.push_back(w_box);
                    word.clear();
                }
                word.push_back(line[i]);
            }

            if (!word.empty()) {
                BBox w_box = { word[0].min_x,    word[0].max_x, word[0].min_y,
                              word[0].max_y,    0.0,           0.0,
                              word[0].max_alpha };
                for (size_t k = 1; k < word.size(); ++k) {
                    w_box.min_x = min(w_box.min_x, word[k].min_x);
                    w_box.max_x = max(w_box.max_x, word[k].max_x);
                    w_box.min_y = min(w_box.min_y, word[k].min_y);
                    w_box.max_y = max(w_box.max_y, word[k].max_y);
                    w_box.max_alpha = max(w_box.max_alpha, word[k].max_alpha);
                }
                result.push_back(w_box);
            }
        }
    }
    else {
        double max_a_whole = 0.0;
        for (const auto& b : boxes) {
            if (b.max_alpha > max_a_whole) {
                max_a_whole = b.max_alpha;
            }
        }
        BBox whole = { (double)g_min_x, (double)g_max_x, (double)g_min_y, (double)g_max_y, 0.0, 0.0, max_a_whole };
        result.push_back(whole);
    }

    // Sort boxes left-to-right for consistent stacking order
    std::sort(result.begin(), result.end(),
        [](const BBox& a, const BBox& b) { return a.min_x < b.min_x; });

    RefineSubpixelBBoxes(result, input, is_deep);

    return result;
}

static PF_Err RenderFunc8(void* refcon, A_long xL, A_long yL, PF_Pixel8* inP,
    PF_Pixel8* outP) {
    if (!refcon || !inP || !outP) {
        return PF_Err_OUT_OF_MEMORY;
    }

    RenderRefcon* rc = (RenderRefcon*)refcon;

    if (xL < rc->render_min_x || xL > rc->render_max_x || yL < rc->render_min_y ||
        yL > rc->render_max_y) {
        *outP = *inP;
        return PF_Err_NONE;
    }

    double text_target_x = (double)xL;
    double text_target_y = (double)yL;

    if (rc->has_layer_transform) {
        double dx = text_target_x - rc->layer_cx;
        double dy = text_target_y - rc->layer_cy;
        text_target_x = rc->layer_cx + dx * rc->cos_total_theta - dy * rc->sin_total_theta;
        text_target_y = rc->layer_cy + dx * rc->sin_total_theta + dy * rc->cos_total_theta;
    }

    if (rc->repeater_enable) {
        double xt = text_target_x - rc->repeater_trans_x;
        double yt = text_target_y - rc->repeater_trans_y;
        double dx = xt - rc->global_cx;
        double dy = yt - rc->global_cy;
        text_target_x = rc->global_cx + dx * rc->repeater_trans_cos + dy * rc->repeater_trans_sin;
        text_target_y = rc->global_cy - dx * rc->repeater_trans_sin + dy * rc->repeater_trans_cos;
    }

    double accum_r = 0.0, accum_g = 0.0, accum_b = 0.0, accum_a = 0.0;

    for (int i = 0; i < rc->num_samples; ++i) {
        double target_x = (double)xL;
        double target_y = (double)yL;

        if (rc->samples[i].has_layer_transform) {
            double dx = target_x - rc->samples[i].layer_cx;
            double dy = target_y - rc->samples[i].layer_cy;
            target_x = rc->samples[i].layer_cx + dx * rc->samples[i].cos_total_theta - dy * rc->samples[i].sin_total_theta;
            target_y = rc->samples[i].layer_cy + dx * rc->samples[i].sin_total_theta + dy * rc->samples[i].cos_total_theta;
        }

        if (rc->repeater_enable) {
            double xt = target_x - rc->repeater_trans_x;
            double yt = target_y - rc->repeater_trans_y;
            double dx = xt - rc->global_cx;
            double dy = yt - rc->global_cy;
            target_x = rc->global_cx + dx * rc->repeater_trans_cos + dy * rc->repeater_trans_sin;
            target_y = rc->global_cy - dx * rc->repeater_trans_sin + dy * rc->repeater_trans_cos;
        }

        double cur_r = 0.0, cur_g = 0.0, cur_b = 0.0, cur_a = 0.0;
        GetBoxCoverage(target_x, target_y, rc, rc->samples[i], &cur_r, &cur_g,
            &cur_b, &cur_a, false);
        accum_r += cur_r * cur_a;
        accum_g += cur_g * cur_a;
        accum_b += cur_b * cur_a;
        accum_a += cur_a;
    }

    double box_r = 0.0, box_g = 0.0, box_b = 0.0, box_a = 0.0;
    if (accum_a > 0.0) {
        box_r = accum_r / accum_a;
        box_g = accum_g / accum_a;
        box_b = accum_b / accum_a;
        box_a = accum_a / rc->num_valid_samples;
    }

    double in_r = inP->red / 255.0;
    double in_g = inP->green / 255.0;
    double in_b = inP->blue / 255.0;
    double in_a = inP->alpha / 255.0;

    if (rc->repeater_enable && (rc->repeater_trans_x != 0.0 || rc->repeater_trans_y != 0.0 || rc->repeater_trans_cos != 1.0)) {
        in_r = 0.0; in_g = 0.0; in_b = 0.0; in_a = 0.0;
        if (text_target_x >= 0.0 && text_target_x < rc->input->width &&
            text_target_y >= 0.0 && text_target_y < rc->input->height) {
            sample_bilinear_xy<PF_Pixel8, PF_Pixel8>(rc->input, text_target_x, text_target_y, &in_r, &in_g, &in_b, &in_a, 255.0);
        }
    }

    double out_r = 0.0, out_g = 0.0, out_b = 0.0, out_a = 0.0;

    if (rc->comp_mode == MGA_COMP_BEHIND) {
        out_a = in_a + box_a * (1.0 - in_a);
        if (out_a > 0.0) {
            out_r = (in_r * in_a + box_r * box_a * (1.0 - in_a)) / out_a;
            out_g = (in_g * in_a + box_g * box_a * (1.0 - in_a)) / out_a;
            out_b = (in_b * in_a + box_b * box_a * (1.0 - in_a)) / out_a;
        }
    }
    else if (rc->comp_mode == MGA_COMP_ON_TOP) {
        out_a = box_a + in_a * (1.0 - box_a);
        if (out_a > 0.0) {
            out_r = (box_r * box_a + in_r * in_a * (1.0 - box_a)) / out_a;
            out_g = (box_g * box_a + in_g * in_a * (1.0 - box_a)) / out_a;
            out_b = (box_b * box_a + in_b * in_a * (1.0 - box_a)) / out_a;
        }
    }
    else { // MGA_COMP_BOX_ONLY
        out_a = box_a;
        out_r = box_r;
        out_g = box_g;
        out_b = box_b;
    }

    outP->alpha = (A_u_char)clamped_channel(out_a * 255.0, 255.0);
    outP->red = (A_u_char)clamped_channel(out_r * 255.0, 255.0);
    outP->green = (A_u_char)clamped_channel(out_g * 255.0, 255.0);
    outP->blue = (A_u_char)clamped_channel(out_b * 255.0, 255.0);

    return PF_Err_NONE;
}

static PF_Err RenderFunc16(void* refcon, A_long xL, A_long yL, PF_Pixel16* inP,
    PF_Pixel16* outP) {
    if (!refcon || !inP || !outP) {
        return PF_Err_OUT_OF_MEMORY;
    }

    RenderRefcon* rc = (RenderRefcon*)refcon;

    if (xL < rc->render_min_x || xL > rc->render_max_x || yL < rc->render_min_y ||
        yL > rc->render_max_y) {
        *outP = *inP;
        return PF_Err_NONE;
    }

    double text_target_x = (double)xL;
    double text_target_y = (double)yL;

    if (rc->has_layer_transform) {
        double dx = text_target_x - rc->layer_cx;
        double dy = text_target_y - rc->layer_cy;
        text_target_x = rc->layer_cx + dx * rc->cos_total_theta - dy * rc->sin_total_theta;
        text_target_y = rc->layer_cy + dx * rc->sin_total_theta + dy * rc->cos_total_theta;
    }

    if (rc->repeater_enable) {
        double xt = text_target_x - rc->repeater_trans_x;
        double yt = text_target_y - rc->repeater_trans_y;
        double dx = xt - rc->global_cx;
        double dy = yt - rc->global_cy;
        text_target_x = rc->global_cx + dx * rc->repeater_trans_cos + dy * rc->repeater_trans_sin;
        text_target_y = rc->global_cy - dx * rc->repeater_trans_sin + dy * rc->repeater_trans_cos;
    }

    double accum_r = 0.0, accum_g = 0.0, accum_b = 0.0, accum_a = 0.0;

    for (int i = 0; i < rc->num_samples; ++i) {
        double target_x = (double)xL;
        double target_y = (double)yL;

        if (rc->samples[i].has_layer_transform) {
            double dx = target_x - rc->samples[i].layer_cx;
            double dy = target_y - rc->samples[i].layer_cy;
            target_x = rc->samples[i].layer_cx + dx * rc->samples[i].cos_total_theta - dy * rc->samples[i].sin_total_theta;
            target_y = rc->samples[i].layer_cy + dx * rc->samples[i].sin_total_theta + dy * rc->samples[i].cos_total_theta;
        }

        if (rc->repeater_enable) {
            double xt = target_x - rc->repeater_trans_x;
            double yt = target_y - rc->repeater_trans_y;
            double dx = xt - rc->global_cx;
            double dy = yt - rc->global_cy;
            target_x = rc->global_cx + dx * rc->repeater_trans_cos + dy * rc->repeater_trans_sin;
            target_y = rc->global_cy - dx * rc->repeater_trans_sin + dy * rc->repeater_trans_cos;
        }

        double cur_r = 0.0, cur_g = 0.0, cur_b = 0.0, cur_a = 0.0;
        GetBoxCoverage(target_x, target_y, rc, rc->samples[i], &cur_r, &cur_g,
            &cur_b, &cur_a, true);
        accum_r += cur_r * cur_a;
        accum_g += cur_g * cur_a;
        accum_b += cur_b * cur_a;
        accum_a += cur_a;
    }

    double box_r = 0.0, box_g = 0.0, box_b = 0.0, box_a = 0.0;
    if (accum_a > 0.0) {
        box_r = accum_r / accum_a;
        box_g = accum_g / accum_a;
        box_b = accum_b / accum_a;
        box_a = accum_a / rc->num_valid_samples;
    }

    double in_r = inP->red / 32768.0;
    double in_g = inP->green / 32768.0;
    double in_b = inP->blue / 32768.0;
    double in_a = inP->alpha / 32768.0;

    if (rc->repeater_enable && (rc->repeater_trans_x != 0.0 || rc->repeater_trans_y != 0.0 || rc->repeater_trans_cos != 1.0)) {
        in_r = 0.0; in_g = 0.0; in_b = 0.0; in_a = 0.0;
        if (text_target_x >= 0.0 && text_target_x < rc->input->width &&
            text_target_y >= 0.0 && text_target_y < rc->input->height) {
            sample_bilinear_xy<PF_Pixel16, PF_Pixel16>(rc->input, text_target_x, text_target_y, &in_r, &in_g, &in_b, &in_a, 32768.0);
        }
    }

    double out_r = 0.0, out_g = 0.0, out_b = 0.0, out_a = 0.0;

    if (rc->comp_mode == MGA_COMP_BEHIND) {
        out_a = in_a + box_a * (1.0 - in_a);
        if (out_a > 0.0) {
            out_r = (in_r * in_a + box_r * box_a * (1.0 - in_a)) / out_a;
            out_g = (in_g * in_a + box_g * box_a * (1.0 - in_a)) / out_a;
            out_b = (in_b * in_a + box_b * box_a * (1.0 - in_a)) / out_a;
        }
    }
    else if (rc->comp_mode == MGA_COMP_ON_TOP) {
        out_a = box_a + in_a * (1.0 - box_a);
        if (out_a > 0.0) {
            out_r = (box_r * box_a + in_r * in_a * (1.0 - box_a)) / out_a;
            out_g = (box_g * box_a + in_g * in_a * (1.0 - box_a)) / out_a;
            out_b = (box_b * box_a + in_b * in_a * (1.0 - box_a)) / out_a;
        }
    }
    else { // MGA_COMP_BOX_ONLY
        out_a = box_a;
        out_r = box_r;
        out_g = box_g;
        out_b = box_b;
    }

    outP->alpha = (A_u_short)clamped_channel(out_a * 32768.0, 32768.0);
    outP->red = (A_u_short)clamped_channel(out_r * 32768.0, 32768.0);
    outP->green = (A_u_short)clamped_channel(out_g * 32768.0, 32768.0);
    outP->blue = (A_u_short)clamped_channel(out_b * 32768.0, 32768.0);

    return PF_Err_NONE;
}

static PF_Err About(PF_InData* in_data, PF_OutData* out_data,
    PF_ParamDef* params[], PF_LayerDef* output) {
    AEGP_SuiteHandler suites(in_data->pica_basicP);

    suites.ANSICallbacksSuite1()->sprintf(out_data->return_msg, "%s v%d.%d\r%s",
        STR(StrID_Name), MAJOR_VERSION,
        MINOR_VERSION, STR(StrID_Description));

    return PF_Err_NONE;
}

static PF_Err GlobalSetup(PF_InData* in_data, PF_OutData* out_data,
    PF_ParamDef* params[], PF_LayerDef* output) {
    out_data->my_version = PF_VERSION(MAJOR_VERSION, MINOR_VERSION, BUG_VERSION,
        STAGE_VERSION, BUILD_VERSION);

    out_data->out_flags =
        PF_OutFlag_DEEP_COLOR_AWARE | PF_OutFlag_I_USE_SHUTTER_ANGLE |
        PF_OutFlag_WIDE_TIME_INPUT | PF_OutFlag_SEND_UPDATE_PARAMS_UI |
        PF_OutFlag_I_EXPAND_BUFFER;
    out_data->out_flags2 = PF_OutFlag2_SUPPORTS_THREADED_RENDERING |
        PF_OutFlag2_AUTOMATIC_WIDE_TIME_INPUT;

    if (s_my_aegp_id == 0) {
        AEGP_SuiteHandler suites(in_data->pica_basicP);
        if (suites.UtilitySuite6()) {
            AEGP_PluginID temp_id = 0;
            suites.UtilitySuite6()->AEGP_RegisterWithAEGP(NULL, "mgAround_AEGP",
                &temp_id);
            s_my_aegp_id = temp_id;
        }
    }

    return PF_Err_NONE;
}

static PF_Err ParamsSetup(PF_InData* in_data, PF_OutData* out_data,
    PF_ParamDef* params[], PF_LayerDef* output) {
    PF_Err err = PF_Err_NONE;
    PF_ParamDef def;

    // NOTE: 7 choices now (was 4) -- Text Outline / Text Solid /
    // Text Outline & Solid were already implemented in GetBoxCoverage() and
    // UpdateParameterUI() but were missing from this popup definition, so
    // they never showed up in the UI dropdown.
    AEFX_CLR_STRUCT(def);
    PF_ADD_POPUP(STR(StrID_Mode_Param_Name), 7, MGA_MODE_SOLID,
        "Outline|Solid Box|Outline & Solid|Custom Layer|Text "
        "Outline|Text Solid|Text Outline & Solid",
        MODE_DISK_ID);

    AEFX_CLR_STRUCT(def);
    PF_ADD_POPUP(STR(StrID_Target_Param_Name), 3, MGA_TARGET_WHOLE,
        "Whole Text|Per Word|Per Character", TARGET_DISK_ID);

    AEFX_CLR_STRUCT(def);
    PF_ADD_CHECKBOX(STR(StrID_SizeMode_Param_Name), "", FALSE, 0,
        SIZE_MODE_DISK_ID);

    AEFX_CLR_STRUCT(def);
    PF_ADD_COLOR(STR(StrID_BoxColor_Param_Name), 255, 0, 0, BOX_COLOR_DISK_ID);

    AEFX_CLR_STRUCT(def);
    PF_ADD_FLOAT_SLIDERX(STR(StrID_BoxOpacity_Param_Name), MGA_BOX_OPACITY_MIN,
        MGA_BOX_OPACITY_MAX, MGA_BOX_OPACITY_MIN,
        MGA_BOX_OPACITY_MAX, MGA_BOX_OPACITY_DFLT,
        PF_Precision_INTEGER, 0, 0, BOX_OPACITY_DISK_ID);

    AEFX_CLR_STRUCT(def);
    PF_ADD_COLOR(STR(StrID_OutlineColor_Param_Name), 0, 0, 0,
        OUTLINE_COLOR_DISK_ID);

    AEFX_CLR_STRUCT(def);
    PF_ADD_FLOAT_SLIDERX(STR(StrID_OutlineOpacity_Param_Name),
        MGA_BOX_OPACITY_MIN, MGA_BOX_OPACITY_MAX,
        MGA_BOX_OPACITY_MIN, MGA_BOX_OPACITY_MAX,
        MGA_BOX_OPACITY_DFLT, PF_Precision_INTEGER, 0, 0,
        OUTLINE_OPACITY_DISK_ID);

    AEFX_CLR_STRUCT(def);
    PF_ADD_FLOAT_SLIDERX(STR(StrID_OutlineWidth_Param_Name),
        MGA_OUTLINE_WIDTH_MIN, MGA_OUTLINE_WIDTH_MAX,
        MGA_OUTLINE_WIDTH_MIN, 20, MGA_OUTLINE_WIDTH_DFLT,
        PF_Precision_TENTHS, 0, 0, OUTLINE_WIDTH_DISK_ID);

    AEFX_CLR_STRUCT(def);
    PF_ADD_FLOAT_SLIDERX(STR(StrID_BorderRadius_Param_Name),
        MGA_BORDER_RADIUS_MIN, MGA_BORDER_RADIUS_MAX,
        MGA_BORDER_RADIUS_MIN, 100, MGA_BORDER_RADIUS_DFLT,
        PF_Precision_TENTHS, 0, 0, BORDER_RADIUS_DISK_ID);

    AEFX_CLR_STRUCT(def);
    PF_ADD_FLOAT_SLIDERX(STR(StrID_PaddingLeft_Param_Name), MGA_PADDING_MIN,
        MGA_PADDING_MAX, -50, 100, MGA_PADDING_DFLT,
        PF_Precision_TENTHS, 0, 0, PADDING_LEFT_DISK_ID);

    AEFX_CLR_STRUCT(def);
    PF_ADD_FLOAT_SLIDERX(STR(StrID_PaddingRight_Param_Name), MGA_PADDING_MIN,
        MGA_PADDING_MAX, -50, 100, MGA_PADDING_DFLT,
        PF_Precision_TENTHS, 0, 0, PADDING_RIGHT_DISK_ID);

    AEFX_CLR_STRUCT(def);
    PF_ADD_FLOAT_SLIDERX(STR(StrID_PaddingTop_Param_Name), MGA_PADDING_MIN,
        MGA_PADDING_MAX, -50, 100, MGA_PADDING_DFLT,
        PF_Precision_TENTHS, 0, 0, PADDING_TOP_DISK_ID);

    AEFX_CLR_STRUCT(def);
    PF_ADD_FLOAT_SLIDERX(STR(StrID_PaddingBottom_Param_Name), MGA_PADDING_MIN,
        MGA_PADDING_MAX, -50, 100, MGA_PADDING_DFLT,
        PF_Precision_TENTHS, 0, 0, PADDING_BOTTOM_DISK_ID);

    AEFX_CLR_STRUCT(def);
    PF_ADD_LAYER(STR(StrID_CustomLayer_Param_Name), PF_LayerDefault_NONE,
        CUSTOM_LAYER_DISK_ID);

    AEFX_CLR_STRUCT(def);
    PF_ADD_CHECKBOX(STR(StrID_UniformScale_Param_Name), "", TRUE, 0,
        UNIFORM_SCALE_DISK_ID);

    AEFX_CLR_STRUCT(def);
    PF_ADD_CHECKBOX(STR(StrID_KeepAspect_Param_Name), "", TRUE, 0,
        KEEP_ASPECT_DISK_ID);

    AEFX_CLR_STRUCT(def);
    PF_ADD_FLOAT_SLIDERX(STR(StrID_ScaleX_Param_Name), MGA_SCALE_MIN,
        MGA_SCALE_MAX, 0, 200, MGA_SCALE_DFLT,
        PF_Precision_INTEGER, 0, 0, SCALE_X_DISK_ID);

    AEFX_CLR_STRUCT(def);
    PF_ADD_FLOAT_SLIDERX(STR(StrID_ScaleY_Param_Name), MGA_SCALE_MIN,
        MGA_SCALE_MAX, 0, 200, MGA_SCALE_DFLT,
        PF_Precision_INTEGER, 0, 0, SCALE_Y_DISK_ID);

    AEFX_CLR_STRUCT(def);
    PF_ADD_FLOAT_SLIDERX(STR(StrID_OffsetX_Param_Name), MGA_OFFSET_MIN,
        MGA_OFFSET_MAX, -200, 200, MGA_OFFSET_DFLT,
        PF_Precision_TENTHS, 0, 0, OFFSET_X_DISK_ID);

    AEFX_CLR_STRUCT(def);
    PF_ADD_FLOAT_SLIDERX(STR(StrID_OffsetY_Param_Name), MGA_OFFSET_MIN,
        MGA_OFFSET_MAX, -200, 200, MGA_OFFSET_DFLT,
        PF_Precision_TENTHS, 0, 0, OFFSET_Y_DISK_ID);

    AEFX_CLR_STRUCT(def);
    PF_ADD_ANGLE(STR(StrID_Rotate_Param_Name), 0, ROTATE_DISK_ID);

    AEFX_CLR_STRUCT(def);
    PF_ADD_POPUP(STR(StrID_CompMode_Param_Name), 3, MGA_COMP_BEHIND,
        "Behind Text|On Top of Text|Boxes Only", COMP_MODE_DISK_ID);

    AEFX_CLR_STRUCT(def);
    PF_ADD_FLOAT_SLIDERX(STR(StrID_WordGap_Param_Name), MGA_GAP_MIN, MGA_GAP_MAX,
        MGA_GAP_MIN, 2.0, MGA_GAP_WORD_DFLT,
        PF_Precision_HUNDREDTHS, 0, 0, WORD_GAP_DISK_ID);

    AEFX_CLR_STRUCT(def);
    PF_ADD_FLOAT_SLIDERX(STR(StrID_LineGap_Param_Name), MGA_GAP_MIN, MGA_GAP_MAX,
        MGA_GAP_MIN, 2.0, MGA_GAP_LINE_DFLT,
        PF_Precision_HUNDREDTHS, 0, 0, LINE_GAP_DISK_ID);

    AEFX_CLR_STRUCT(def);
    PF_ADD_POPUP(STR(StrID_MBEnable_Param_Name), 3, 2, "Off|Comp Setting|On",
        MB_ENABLE_DISK_ID);

    AEFX_CLR_STRUCT(def);
    PF_ADD_FLOAT_SLIDERX(STR(StrID_MBSamples_Param_Name), 2, 64, 2, 32, 8,
        PF_Precision_INTEGER, 0, 0, MB_SAMPLES_DISK_ID);

    AEFX_CLR_STRUCT(def);
    PF_ADD_CHECKBOX(STR(StrID_FillOutline_Param_Name), "", FALSE, 0,
        FILL_OUTLINE_DISK_ID);

    AEFX_CLR_STRUCT(def);
    PF_ADD_CHECKBOX("Repeater Enable", "", FALSE, 0, REPEATER_ENABLE_DISK_ID);

    AEFX_CLR_STRUCT(def);
    PF_ADD_FLOAT_SLIDERX("Count Left", 0, 50, 0, 50, 0, PF_Precision_INTEGER, 0, 0, REPEATER_COUNT_L_DISK_ID);

    AEFX_CLR_STRUCT(def);
    PF_ADD_FLOAT_SLIDERX("Count Right", 0, 50, 0, 50, 0, PF_Precision_INTEGER, 0, 0, REPEATER_COUNT_R_DISK_ID);

    AEFX_CLR_STRUCT(def);
    PF_ADD_FLOAT_SLIDERX("Count Up", 0, 50, 0, 50, 0, PF_Precision_INTEGER, 0, 0, REPEATER_COUNT_U_DISK_ID);

    AEFX_CLR_STRUCT(def);
    PF_ADD_FLOAT_SLIDERX("Count Down", 0, 50, 0, 50, 0, PF_Precision_INTEGER, 0, 0, REPEATER_COUNT_D_DISK_ID);

    AEFX_CLR_STRUCT(def);
    PF_ADD_FLOAT_SLIDERX("Gap X", -1000, 5000, -100, 500, 10, PF_Precision_TENTHS, 0, 0, REPEATER_GAP_X_DISK_ID);

    AEFX_CLR_STRUCT(def);
    PF_ADD_FLOAT_SLIDERX("Gap Y", -1000, 5000, -100, 500, 10, PF_Precision_TENTHS, 0, 0, REPEATER_GAP_Y_DISK_ID);

    AEFX_CLR_STRUCT(def);
    PF_ADD_FLOAT_SLIDERX("Offset X", -2000, 2000, -200, 200, 0, PF_Precision_TENTHS, 0, 0, REPEATER_OFFSET_X_DISK_ID);

    AEFX_CLR_STRUCT(def);
    PF_ADD_FLOAT_SLIDERX("Offset Y", -2000, 2000, -200, 200, 0, PF_Precision_TENTHS, 0, 0, REPEATER_OFFSET_Y_DISK_ID);

    AEFX_CLR_STRUCT(def);
    PF_ADD_CHECKBOX("Show Text", "", TRUE, 0, REPEATER_SHOW_TEXT_DISK_ID);

    AEFX_CLR_STRUCT(def);
    PF_ADD_CHECKBOX("Show Shape", "", TRUE, 0, REPEATER_SHOW_SHAPE_DISK_ID);

    AEFX_CLR_STRUCT(def);
    PF_ADD_FLOAT_SLIDERX("Repeater Trans X", -5000, 5000, -1000, 1000, 0,
        PF_Precision_TENTHS, 0, 0, REPEATER_TRANS_X_DISK_ID);

    AEFX_CLR_STRUCT(def);
    PF_ADD_FLOAT_SLIDERX("Repeater Trans Y", -5000, 5000, -1000, 1000, 0,
        PF_Precision_TENTHS, 0, 0, REPEATER_TRANS_Y_DISK_ID);

    AEFX_CLR_STRUCT(def);
    PF_ADD_ANGLE("Repeater Trans Rotate", 0, REPEATER_TRANS_ROTATE_DISK_ID);

    out_data->num_params = MGA_NUM_PARAMS;

    return err;
}

static void RenderChunk(int start_y, int end_y, int width, int height,
    PF_EffectWorld* src, PF_LayerDef* output, void* rc,
    bool is_deep) {
    RenderRefcon* rc_cast = (RenderRefcon*)rc;

    int r_min_y = (int)floor(rc_cast->render_min_y);
    int r_max_y = (int)ceil(rc_cast->render_max_y);
    int r_min_x = (int)floor(rc_cast->render_min_x);
    int r_max_x = (int)ceil(rc_cast->render_max_x);

    // Clamp coordinates to image boundaries
    if (r_min_y < 0)
        r_min_y = 0;
    if (r_max_y >= height)
        r_max_y = height - 1;
    if (r_min_x < 0)
        r_min_x = 0;
    if (r_max_x >= width)
        r_max_x = width - 1;

    // If active box is completely invalid or empty, we just copy everything
    // directly
    if (r_min_y > r_max_y || r_min_x > r_max_x) {
        if (is_deep) {
            for (int y = start_y; y < end_y; ++y) {
                PF_Pixel16* in_row =
                    (PF_Pixel16*)((char*)src->data + y * src->rowbytes);
                PF_Pixel16* out_row =
                    (PF_Pixel16*)((char*)output->data + y * output->rowbytes);
                memcpy(out_row, in_row, width * sizeof(PF_Pixel16));
            }
        }
        else {
            for (int y = start_y; y < end_y; ++y) {
                PF_Pixel8* in_row =
                    (PF_Pixel8*)((char*)src->data + y * src->rowbytes);
                PF_Pixel8* out_row =
                    (PF_Pixel8*)((char*)output->data + y * output->rowbytes);
                memcpy(out_row, in_row, width * sizeof(PF_Pixel8));
            }
        }
        return;
    }

    if (is_deep) {
        for (int y = start_y; y < end_y; ++y) {
            PF_Pixel16* in_row =
                (PF_Pixel16*)((char*)src->data + y * src->rowbytes);
            PF_Pixel16* out_row =
                (PF_Pixel16*)((char*)output->data + y * output->rowbytes);

            if (y < r_min_y || y > r_max_y) {
                memcpy(out_row, in_row, width * sizeof(PF_Pixel16));
            }
            else {
                if (r_min_x > 0) {
                    memcpy(out_row, in_row, r_min_x * sizeof(PF_Pixel16));
                }
                for (int x = r_min_x; x <= r_max_x; ++x) {
                    RenderFunc16(rc, x, y, &in_row[x], &out_row[x]);
                }
                if (r_max_x < width - 1) {
                    int right_count = width - 1 - r_max_x;
                    memcpy(out_row + r_max_x + 1, in_row + r_max_x + 1,
                        right_count * sizeof(PF_Pixel16));
                }
            }
        }
    }
    else {
        for (int y = start_y; y < end_y; ++y) {
            PF_Pixel8* in_row = (PF_Pixel8*)((char*)src->data + y * src->rowbytes);
            PF_Pixel8* out_row =
                (PF_Pixel8*)((char*)output->data + y * output->rowbytes);

            if (y < r_min_y || y > r_max_y) {
                memcpy(out_row, in_row, width * sizeof(PF_Pixel8));
            }
            else {
                if (r_min_x > 0) {
                    memcpy(out_row, in_row, r_min_x * sizeof(PF_Pixel8));
                }
                for (int x = r_min_x; x <= r_max_x; ++x) {
                    RenderFunc8(rc, x, y, &in_row[x], &out_row[x]);
                }
                if (r_max_x < width - 1) {
                    int right_count = width - 1 - r_max_x;
                    memcpy(out_row + r_max_x + 1, in_row + r_max_x + 1,
                        right_count * sizeof(PF_Pixel8));
                }
            }
        }
    }
}

static PF_Err CallIterate(PF_InData* in_data, A_long linesL,
    PF_EffectWorld* src, void* rc, PF_LayerDef* output,
    bool is_deep) {
    PF_Err err = PF_Err_NONE;
    int width = output->width;
    int height = output->height;

    RenderRefcon* render_rc = (RenderRefcon*)rc;
    int min_x = max(0, (int)floor(render_rc->render_min_x));
    int max_x = min(width - 1, (int)ceil(render_rc->render_max_x));
    int min_y = max(0, (int)floor(render_rc->render_min_y));
    int max_y = min(height - 1, (int)ceil(render_rc->render_max_y));

    // Copy the frame once, then schedule workers only over rows touched by the
    // effect. The old implementation split the entire frame across all logical
    // CPUs even when the text occupied only a small strip.
    const size_t pixel_size = is_deep ? sizeof(PF_Pixel16) : sizeof(PF_Pixel8);
    if (src->data != output->data || src->rowbytes != output->rowbytes) {
        for (int y = 0; y < height; ++y) {
            void* src_row = (char*)src->data + y * src->rowbytes;
            void* dst_row = (char*)output->data + y * output->rowbytes;
            memcpy(dst_row, src_row, (size_t)width * pixel_size);
        }
    }

    if (min_x > max_x || min_y > max_y) {
        return err;
    }

    unsigned int num_threads = std::thread::hardware_concurrency();
    if (num_threads == 0)
        num_threads = 4;

    // After Effects can already render several frames concurrently. Cap nested
    // workers to avoid oversubscription and size the pool to the active rows.
    const unsigned int active_rows = (unsigned int)(max_y - min_y + 1);
    num_threads = min(num_threads, 8u);
    num_threads = min(num_threads, max(1u, (active_rows + 31u) / 32u));

    std::vector<std::thread> threads;
    threads.reserve(num_threads);

    int chunk_size = ((int)active_rows + (int)num_threads - 1) /
        (int)num_threads;
    if (chunk_size == 0)
        chunk_size = 1;

    for (unsigned int i = 0; i < num_threads; ++i) {
        int start_y = min_y + (int)i * chunk_size;
        int end_y = min(max_y + 1, start_y + chunk_size);
        if (start_y < end_y) {
            threads.emplace_back(RenderChunk, start_y, end_y, width, height, src,
                output, rc, is_deep);
        }
    }

    for (auto& t : threads) {
        if (t.joinable()) {
            t.join();
        }
    }

    return err;
}

static void SumTextAnimatorRotations(
    AEGP_SuiteHandler& suites,
    AEGP_StreamRefH parent_group,
    const A_Time* t,
    double* accum_rotation_rad) {
    A_long num_streams = 0;
    if (suites.DynamicStreamSuite4() && suites.DynamicStreamSuite4()->AEGP_GetNumStreamsInGroup(parent_group, &num_streams) == A_Err_NONE && num_streams > 0) {
        for (A_long i = 0; i < num_streams; ++i) {
            AEGP_StreamRefH streamH = NULL;
            if (suites.DynamicStreamSuite4()->AEGP_GetNewStreamRefByIndex(s_my_aegp_id, parent_group, i, &streamH) == A_Err_NONE && streamH) {
                AEGP_StreamGroupingType grouping_type = AEGP_StreamGroupingType_NONE;
                suites.DynamicStreamSuite4()->AEGP_GetStreamGroupingType(streamH, &grouping_type);

                if (grouping_type == AEGP_StreamGroupingType_NAMED_GROUP || grouping_type == AEGP_StreamGroupingType_INDEXED_GROUP) {
                    SumTextAnimatorRotations(suites, streamH, t, accum_rotation_rad);
                }
                else if (grouping_type == AEGP_StreamGroupingType_LEAF) {
                    char match_name[AEGP_MAX_STREAM_MATCH_NAME_SIZE] = { 0 };
                    if (suites.DynamicStreamSuite4()->AEGP_GetMatchName(streamH, match_name) == A_Err_NONE) {
                        if (strcmp(match_name, "ADBE Text Rotation") == 0) {
                            AEGP_StreamValue2 val;
                            AEFX_CLR_STRUCT(val);
                            if (suites.StreamSuite5()->AEGP_GetNewStreamValue(s_my_aegp_id, streamH, AEGP_LTimeMode_CompTime, t, TRUE, &val) == A_Err_NONE) {
                                *accum_rotation_rad += val.val.one_d * (3.14159265358979323846 / 180.0);
                                suites.StreamSuite5()->AEGP_DisposeStreamValue(&val);
                            }
                        }
                    }
                }
                suites.StreamSuite5()->AEGP_DisposeStream(streamH);
            }
        }
    }
}

static double GetTextLayerAnimatorRotation(
    AEGP_SuiteHandler& suites,
    AEGP_LayerH layerH,
    const A_Time* t) {
    double accum_rotation_rad = 0.0;
    if (suites.DynamicStreamSuite4()) {
        AEGP_StreamRefH root_streamH = NULL;
        if (suites.DynamicStreamSuite4()->AEGP_GetNewStreamRefForLayer(s_my_aegp_id, layerH, &root_streamH) == A_Err_NONE && root_streamH) {
            AEGP_StreamRefH text_prop_groupH = NULL;
            if (suites.DynamicStreamSuite4()->AEGP_GetNewStreamRefByMatchname(s_my_aegp_id, root_streamH, "ADBE Text Properties", &text_prop_groupH) == A_Err_NONE && text_prop_groupH) {
                SumTextAnimatorRotations(suites, text_prop_groupH, t, &accum_rotation_rad);
                suites.StreamSuite5()->AEGP_DisposeStream(text_prop_groupH);
            }
            suites.StreamSuite5()->AEGP_DisposeStream(root_streamH);
        }
    }
    return accum_rotation_rad;
}

static bool GetTextVectorBoxes(
    PF_InData* in_data,
    AEGP_SuiteHandler& suites,
    AEGP_LayerH layerH,
    const A_Time* comp_time,
    const PF_EffectWorld* input_world,
    std::vector<BBox>& out_boxes) {
    out_boxes.clear();
    if (!layerH || !suites.TextLayerSuite1() || !suites.PathDataSuite1() || !input_world) {
        return false;
    }

    AEGP_TextOutlinesH outlinesH = NULL;
    A_Err err = suites.TextLayerSuite1()->AEGP_GetNewTextOutlines(layerH, comp_time, &outlinesH);
    if (err != A_Err_NONE || !outlinesH) {
        return false;
    }

    A_long num_outlines = 0;
    suites.TextLayerSuite1()->AEGP_GetNumTextOutlines(outlinesH, &num_outlines);

    double scale_x = (double)in_data->downsample_x.num / in_data->downsample_x.den;
    double scale_y = (double)in_data->downsample_y.num / in_data->downsample_y.den;

    bool is_deep = PF_WORLD_IS_DEEP(input_world);
    int img_w = input_world->width;
    int img_h = input_world->height;

    for (A_long i = 0; i < num_outlines; ++i) {
        PF_PathOutlinePtr pathP = NULL;
        if (suites.TextLayerSuite1()->AEGP_GetIndexedTextOutline(outlinesH, i, &pathP) == A_Err_NONE && pathP) {
            A_long num_segments = 0;
            if (suites.PathDataSuite1()->PF_PathNumSegments(in_data->effect_ref, pathP, &num_segments) == A_Err_NONE && num_segments > 0) {
                double b_min_x = 999999.0, b_max_x = -999999.0;
                double b_min_y = 999999.0, b_max_y = -999999.0;
                bool valid_vertex = false;

                for (A_long v = 0; v <= num_segments; ++v) {
                    PF_PathVertex vert;
                    if (suites.PathDataSuite1()->PF_PathVertexInfo(in_data->effect_ref, pathP, v, &vert) == A_Err_NONE) {
                        double vx = vert.x * scale_x;
                        double vy = vert.y * scale_y;
                        if (vx < b_min_x) b_min_x = vx;
                        if (vx > b_max_x) b_max_x = vx;
                        if (vy < b_min_y) b_min_y = vy;
                        if (vy > b_max_y) b_max_y = vy;
                        valid_vertex = true;
                    }
                }

                if (valid_vertex && b_max_x >= b_min_x && b_max_y >= b_min_y) {
                    // Check rendered alpha of this character in input_world
                    double char_max_alpha = 0.0;
                    int x0 = max(0, (int)floor(b_min_x));
                    int x1 = min(img_w - 1, (int)ceil(b_max_x));
                    int y0 = max(0, (int)floor(b_min_y));
                    int y1 = min(img_h - 1, (int)ceil(b_max_y));

                    for (int sy = y0; sy <= y1; ++sy) {
                        void* row_ptr = (char*)input_world->data + sy * input_world->rowbytes;
                        for (int sx = x0; sx <= x1; ++sx) {
                            double a = is_deep 
                                ? (((PF_Pixel16*)row_ptr)[sx].alpha / 32768.0)
                                : (((PF_Pixel8*)row_ptr)[sx].alpha / 255.0);
                            if (a > char_max_alpha) {
                                char_max_alpha = a;
                            }
                        }
                    }

                    // Skip hidden characters (Opacity Animator set alpha to 0)
                    if (char_max_alpha > 0.02) {
                        BBox char_box;
                        char_box.min_x = b_min_x;
                        char_box.max_x = b_max_x;
                        char_box.min_y = b_min_y;
                        char_box.max_y = b_max_y;
                        char_box.cx = (b_min_x + b_max_x) / 2.0;
                        char_box.cy = (b_min_y + b_max_y) / 2.0;
                        char_box.max_alpha = char_max_alpha;
                        out_boxes.push_back(char_box);
                    }
                }
            }
        }
    }

    suites.TextLayerSuite1()->AEGP_DisposeTextOutlines(outlinesH);
    return !out_boxes.empty();
}

struct SampleTransform {
    bool has_layer_transform;
    double layer_rotation_rad;
    double layer_cx;
    double layer_cy;
    double animator_rotation_rad;
    A_Matrix4 layer_matrix;
};

struct FrameData {
    bool is_text;
    bool is_collapsed;
    std::vector<SampleTransform> samples;
};

static void GetMotionBlurSettings(
    PF_InData* in_data,
    AEGP_SuiteHandler& suites,
    AEGP_LayerH layerH,
    int mb_enable,
    int mb_samples_param,
    bool& do_mb,
    int& effective_samples,
    double& shutter_angle_deg,
    double& shutter_phase_deg) {
    do_mb = false;
    effective_samples = 1;
    shutter_angle_deg = 180.0;
    shutter_phase_deg = -90.0;

    if (mb_enable == 1) { // Off
        do_mb = false;
    }
    else if (mb_enable == 2) { // Comp Setting
        bool is_layer_mb_on = false;
        int comp_samples = 16;
        double comp_shutter_angle = 180.0;
        double comp_shutter_phase = -90.0;

        if (suites.PFInterfaceSuite1() && suites.LayerSuite8() && suites.CompSuite11() && layerH) {
            AEGP_LayerFlags lflags = 0;
            suites.LayerSuite8()->AEGP_GetLayerFlags(layerH, &lflags);
            is_layer_mb_on = (lflags & AEGP_LayerFlag_MOTION_BLUR) != 0;

            AEGP_CompH compH = NULL;
            suites.LayerSuite8()->AEGP_GetLayerParentComp(layerH, &compH);
            if (compH) {
                A_long samples_val = 0;
                A_Err s_err = suites.CompSuite11()->AEGP_GetCompSuggestedMotionBlurSamples(compH, &samples_val);
                if (!s_err && samples_val > 0) {
                    comp_samples = (int)samples_val;
                }
                A_Ratio angle, phase;
                A_Err ap_err = suites.CompSuite11()->AEGP_GetCompShutterAnglePhase(compH, &angle, &phase);
                if (!ap_err) {
                    if (angle.den != 0)
                        comp_shutter_angle = (double)angle.num / (double)angle.den;
                    if (phase.den != 0)
                        comp_shutter_phase = (double)phase.num / (double)phase.den;
                }
            }
        }

        if (is_layer_mb_on && in_data->shutter_angle > 0) {
            do_mb = true;
            effective_samples = comp_samples;
            if (effective_samples > 16) {
                effective_samples = 16;
            }
            shutter_angle_deg = comp_shutter_angle * 360.0;
            shutter_phase_deg = comp_shutter_phase * 360.0;
        }
    }
    else if (mb_enable == 3) { // On
        do_mb = true;
        effective_samples = mb_samples_param;
        if (effective_samples > 16) {
            effective_samples = 16;
        }
        shutter_angle_deg = (double)in_data->shutter_angle / 256.0;
        shutter_phase_deg = (double)in_data->shutter_phase / 256.0;
        if (shutter_angle_deg <= 0.0) {
            shutter_angle_deg = 180.0;
            shutter_phase_deg = -90.0;
        }
    }
}

static PF_Err FrameSetup(PF_InData* in_data, PF_OutData* out_data,
    PF_ParamDef* params[], PF_LayerDef* output) {
    PF_Err err = PF_Err_NONE;
    out_data->width = params[MGA_INPUT]->u.ld.width;
    out_data->height = params[MGA_INPUT]->u.ld.height;
    MGA_LOG("FrameSetup entry: out_data size set to " + std::to_string(out_data->width) + "x" + std::to_string(out_data->height));

    AEGP_SuiteHandler suites(in_data->pica_basicP);

    if (!suites.HandleSuite1()) {
        MGA_LOG("FrameSetup ERR: HandleSuite1 is NULL");
        return PF_Err_INTERNAL_STRUCT_DAMAGED;
    }

    PF_Handle handle = suites.HandleSuite1()->host_new_handle(sizeof(FrameData*));
    if (!handle) {
        MGA_LOG("FrameSetup ERR: host_new_handle returned NULL");
        return PF_Err_OUT_OF_MEMORY;
    }

    FrameData** fd_ptr = (FrameData**)suites.HandleSuite1()->host_lock_handle(handle);
    if (!fd_ptr) {
        MGA_LOG("FrameSetup ERR: host_lock_handle returned NULL");
        suites.HandleSuite1()->host_dispose_handle(handle);
        return PF_Err_OUT_OF_MEMORY;
    }

    FrameData* fd = new FrameData();
    *fd_ptr = fd;

    fd->is_text = false;
    fd->is_collapsed = false;

    AEGP_LayerH layerH = NULL;
    if (suites.PFInterfaceSuite1() && suites.LayerSuite8()) {
        A_Err a_err = suites.PFInterfaceSuite1()->AEGP_GetEffectLayer(in_data->effect_ref, &layerH);
        MGA_LOG("FrameSetup: AEGP_GetEffectLayer returned " + std::to_string(a_err) + ", layerH = " + std::to_string((uintptr_t)layerH));
        if (!a_err && layerH) {
            AEGP_ObjectType obj_type = AEGP_ObjectType_NONE;
            AEGP_LayerFlags flags = 0;
            if (!suites.LayerSuite8()->AEGP_GetLayerObjectType(layerH, &obj_type)) {
                fd->is_text = (obj_type == AEGP_ObjectType_TEXT);
            }
            if (!suites.LayerSuite8()->AEGP_GetLayerFlags(layerH, &flags)) {
                fd->is_collapsed = ((flags & AEGP_LayerFlag_COLLAPSE) != 0);
            }
        }
    }
    MGA_LOG("FrameSetup: is_text = " + std::to_string(fd->is_text) + ", is_collapsed = " + std::to_string(fd->is_collapsed));

    // Calculate sample times (motion blur)
    std::vector<A_long> sample_times;
    int mb_enable = params[MGA_MB_ENABLE]->u.pd.value;
    int mb_samples_param = (int)params[MGA_MB_SAMPLES]->u.fs_d.value;

    bool do_mb = false;
    int effective_samples = 1;
    double shutter_angle_deg = 180.0;
    double shutter_phase_deg = -90.0;
    GetMotionBlurSettings(in_data, suites, layerH, mb_enable, mb_samples_param, do_mb, effective_samples, shutter_angle_deg, shutter_phase_deg);
    MGA_LOG("FrameSetup: mb_enable = " + std::to_string(mb_enable) + ", mb_samples_param = " + std::to_string(mb_samples_param) + ", effective_samples = " + std::to_string(effective_samples));

    if (effective_samples == 1) {
        sample_times.push_back(in_data->current_time);
    }
    else {
        double d_shutter = (double)in_data->time_step * (shutter_angle_deg / 360.0);
        double t_start = (double)in_data->current_time +
            (double)in_data->time_step * (shutter_phase_deg / 360.0);
        for (int i = 0; i < effective_samples; ++i) {
            double frac = (double)i / (effective_samples - 1);
            double t = t_start + d_shutter * frac;
            A_long t_i = (A_long)round(t);
            if (t_i < 0) t_i = 0;
            sample_times.push_back(t_i);
        }
    }
    MGA_LOG("FrameSetup: sample_times.size = " + std::to_string(sample_times.size()));

    double scale_x = (double)in_data->width / (double)params[MGA_INPUT]->u.ld.width;
    double scale_y = (double)in_data->height / (double)params[MGA_INPUT]->u.ld.height;

    fd->samples.resize(sample_times.size());
    for (size_t i = 0; i < sample_times.size(); ++i) {
        A_long t_i = sample_times[i];
        A_Time t_time = { t_i, in_data->time_scale };

        SampleTransform& st = fd->samples[i];
        st.has_layer_transform = false;
        st.layer_rotation_rad = 0.0;
        st.layer_cx = 0.0;
        st.layer_cy = 0.0;
        st.animator_rotation_rad = 0.0;
        AEFX_CLR_STRUCT(st.layer_matrix);

        if (layerH && (fd->is_text || fd->is_collapsed)) {
            A_Time comp_time = t_time;
            if (suites.LayerSuite8()) {
                suites.LayerSuite8()->AEGP_ConvertLayerToCompTime(layerH, &t_time, &comp_time);
            }
            A_Err xform_err = suites.LayerSuite8()->AEGP_GetLayerToWorldXform(layerH, &comp_time, &st.layer_matrix);
            MGA_LOG("FrameSetup: AEGP_GetLayerToWorldXform returned " + std::to_string(xform_err));
            if (!xform_err) {
                st.layer_rotation_rad = atan2(st.layer_matrix.mat[0][1], st.layer_matrix.mat[0][0]);

                double anchor_x = 0.0;
                double anchor_y = 0.0;
                AEGP_StreamRefH anchor_streamH = NULL;
                if (suites.StreamSuite5() && !suites.StreamSuite5()->AEGP_GetNewLayerStream(s_my_aegp_id, layerH, AEGP_LayerStream_ANCHORPOINT, &anchor_streamH) && anchor_streamH) {
                    AEGP_StreamValue2 val;
                    AEFX_CLR_STRUCT(val);
                    if (!suites.StreamSuite5()->AEGP_GetNewStreamValue(s_my_aegp_id, anchor_streamH, AEGP_LTimeMode_CompTime, &comp_time, TRUE, &val)) {
                        anchor_x = val.val.three_d.x;
                        anchor_y = val.val.three_d.y;
                        suites.StreamSuite5()->AEGP_DisposeStreamValue(&val);
                    }
                    suites.StreamSuite5()->AEGP_DisposeStream(anchor_streamH);
                }
                double downsample_x = (double)in_data->downsample_x.num / in_data->downsample_x.den;
                double downsample_y = (double)in_data->downsample_y.num / in_data->downsample_y.den;
                double world_cx_full = anchor_x * st.layer_matrix.mat[0][0] + anchor_y * st.layer_matrix.mat[1][0] + st.layer_matrix.mat[3][0];
                double world_cy_full = anchor_x * st.layer_matrix.mat[0][1] + anchor_y * st.layer_matrix.mat[1][1] + st.layer_matrix.mat[3][1];
                st.layer_cx = world_cx_full * downsample_x;
                st.layer_cy = world_cy_full * downsample_y;
                MGA_LOG("FrameSetup details: anchor_x=" + std::to_string(anchor_x) +
                    ", anchor_y=" + std::to_string(anchor_y) +
                    ", origin_x=" + std::to_string(params[MGA_INPUT]->u.ld.origin_x) +
                    ", origin_y=" + std::to_string(params[MGA_INPUT]->u.ld.origin_y) +
                    ", pre_effect_source_origin_x=" + std::to_string(in_data->pre_effect_source_origin_x) +
                    ", pre_effect_source_origin_y=" + std::to_string(in_data->pre_effect_source_origin_y) +
                    ", output_origin_x=" + std::to_string(in_data->output_origin_x) +
                    ", output_origin_y=" + std::to_string(in_data->output_origin_y) +
                    ", input_width=" + std::to_string(params[MGA_INPUT]->u.ld.width) +
                    ", input_height=" + std::to_string(params[MGA_INPUT]->u.ld.height) +
                    ", comp_width=" + std::to_string(in_data->width) +
                    ", comp_height=" + std::to_string(in_data->height));

                if (fd->is_text) {
                    st.animator_rotation_rad = GetTextLayerAnimatorRotation(suites, layerH, &comp_time);
                }
                st.has_layer_transform = true;
            }
        }
    }

    suites.HandleSuite1()->host_unlock_handle(handle);
    out_data->frame_data = handle;
    MGA_LOG("FrameSetup successful exit. handle set.");
    return err;
}

static PF_Err FrameSetdown(PF_InData* in_data, PF_OutData* out_data,
    PF_ParamDef* params[], PF_LayerDef* output) {
    PF_Err err = PF_Err_NONE;
    AEGP_SuiteHandler suites(in_data->pica_basicP);

    if (in_data->frame_data && suites.HandleSuite1()) {
        FrameData** fd_ptr = (FrameData**)suites.HandleSuite1()->host_lock_handle(in_data->frame_data);
        if (fd_ptr && *fd_ptr) {
            delete* fd_ptr;
        }
        suites.HandleSuite1()->host_unlock_handle(in_data->frame_data);
        suites.HandleSuite1()->host_dispose_handle(in_data->frame_data);
        out_data->frame_data = NULL;
    }
    return err;
}

static PF_Err Render(PF_InData* in_data, PF_OutData* out_data,
    PF_ParamDef* params[], PF_LayerDef* output) {
    PF_Err err = PF_Err_NONE, err2 = PF_Err_NONE;
    AEGP_SuiteHandler suites(in_data->pica_basicP);

    double layer_rotation_rad = 0.0;
    double layer_cx = 0.0;
    double layer_cy = 0.0;
    bool has_layer_transform = false;
    bool is_text = false;
    AEGP_LayerH layerH = NULL;
    A_Matrix4 layer_matrix;
    AEFX_CLR_STRUCT(layer_matrix);

    FrameData* fd = NULL;
    if (in_data->frame_data && suites.HandleSuite1()) {
        FrameData** fd_ptr = (FrameData**)suites.HandleSuite1()->host_lock_handle(in_data->frame_data);
        if (fd_ptr) {
            fd = *fd_ptr;
        }
    }

    MGA_LOG("Render: in_data->frame_data = " + std::to_string((uintptr_t)in_data->frame_data) + ", fd = " + std::to_string((uintptr_t)fd));

    if (fd) {
        MGA_LOG("Render: Using fd! samples count = " + std::to_string(fd->samples.size()));
        is_text = fd->is_text;
        int current_sample_idx = -1;
        int mb_enable = params[MGA_MB_ENABLE]->u.pd.value;
        int mb_samples_param = (int)params[MGA_MB_SAMPLES]->u.fs_d.value;

        AEGP_LayerH main_layerH = NULL;
        if (suites.PFInterfaceSuite1()) {
            suites.PFInterfaceSuite1()->AEGP_GetEffectLayer(in_data->effect_ref, &main_layerH);
        }

        bool do_mb = false;
        int effective_samples = 1;
        double shutter_angle_deg = 180.0;
        double shutter_phase_deg = -90.0;
        GetMotionBlurSettings(in_data, suites, main_layerH, mb_enable, mb_samples_param, do_mb, effective_samples, shutter_angle_deg, shutter_phase_deg);

        std::vector<A_long> local_times;
        if (effective_samples == 1) {
            local_times.push_back(in_data->current_time);
        }
        else {
            double d_shutter = (double)in_data->time_step * (shutter_angle_deg / 360.0);
            double t_start = (double)in_data->current_time + (double)in_data->time_step * (shutter_phase_deg / 360.0);
            for (int i = 0; i < effective_samples; ++i) {
                double frac = (double)i / (effective_samples - 1);
                double t = t_start + d_shutter * frac;
                A_long t_i = (A_long)round(t);
                if (t_i < 0) t_i = 0;
                local_times.push_back(t_i);
            }
        }

        for (size_t i = 0; i < local_times.size(); ++i) {
            if (local_times[i] == in_data->current_time) {
                current_sample_idx = (int)i;
                break;
            }
        }
        if (current_sample_idx == -1 && !fd->samples.empty()) {
            current_sample_idx = 0;
        }

        if (current_sample_idx >= 0 && current_sample_idx < (int)fd->samples.size()) {
            const auto& st = fd->samples[current_sample_idx];
            layer_rotation_rad = st.layer_rotation_rad;
            layer_cx = st.layer_cx;
            layer_cy = st.layer_cy;
            has_layer_transform = st.has_layer_transform;
            layer_matrix = st.layer_matrix;
        }
    }
    else {
        MGA_LOG("Render: fd is NULL! Using fallback.");
        if (suites.PFInterfaceSuite1() && suites.LayerSuite8()) {
            A_Err a_err = suites.PFInterfaceSuite1()->AEGP_GetEffectLayer(in_data->effect_ref, &layerH);
            if (!a_err && layerH) {
                AEGP_ObjectType obj_type = AEGP_ObjectType_NONE;
                AEGP_LayerFlags flags = 0;
                bool is_collapsed = false;
                if (!suites.LayerSuite8()->AEGP_GetLayerObjectType(layerH, &obj_type)) {
                    is_text = (obj_type == AEGP_ObjectType_TEXT);
                }
                if (!suites.LayerSuite8()->AEGP_GetLayerFlags(layerH, &flags)) {
                    is_collapsed = ((flags & AEGP_LayerFlag_COLLAPSE) != 0);
                }

                if (is_text || is_collapsed) {
                    A_Time t = { in_data->current_time, in_data->time_scale };
                    A_Time comp_time = t;
                    if (suites.LayerSuite8()) {
                        suites.LayerSuite8()->AEGP_ConvertLayerToCompTime(layerH, &t, &comp_time);
                    }
                    if (!suites.LayerSuite8()->AEGP_GetLayerToWorldXform(layerH, &comp_time, &layer_matrix)) {
                        layer_rotation_rad = atan2(layer_matrix.mat[0][1], layer_matrix.mat[0][0]);
                        double anchor_x = 0.0;
                        double anchor_y = 0.0;
                        AEGP_StreamRefH anchor_streamH = NULL;
                        if (suites.StreamSuite5() && !suites.StreamSuite5()->AEGP_GetNewLayerStream(s_my_aegp_id, layerH, AEGP_LayerStream_ANCHORPOINT, &anchor_streamH) && anchor_streamH) {
                            AEGP_StreamValue2 val;
                            AEFX_CLR_STRUCT(val);
                            if (!suites.StreamSuite5()->AEGP_GetNewStreamValue(s_my_aegp_id, anchor_streamH, AEGP_LTimeMode_CompTime, &comp_time, TRUE, &val)) {
                                anchor_x = val.val.three_d.x;
                                anchor_y = val.val.three_d.y;
                                suites.StreamSuite5()->AEGP_DisposeStreamValue(&val);
                            }
                            suites.StreamSuite5()->AEGP_DisposeStream(anchor_streamH);
                        }
                        double downsample_x = (double)in_data->downsample_x.num / in_data->downsample_x.den;
                        double downsample_y = (double)in_data->downsample_y.num / in_data->downsample_y.den;
                        double world_cx_full = anchor_x * layer_matrix.mat[0][0] + anchor_y * layer_matrix.mat[1][0] + layer_matrix.mat[3][0];
                        double world_cy_full = anchor_x * layer_matrix.mat[0][1] + anchor_y * layer_matrix.mat[1][1] + layer_matrix.mat[3][1];
                        layer_cx = world_cx_full * downsample_x;
                        layer_cy = world_cy_full * downsample_y;
                        has_layer_transform = true;
                    }
                }
            }
        }
    }

    int mode = params[MGA_MODE]->u.pd.value;
    int target = params[MGA_TARGET]->u.pd.value;
    bool consistent_size = (params[MGA_SIZE_MODE]->u.bd.value != 0);
    PF_Pixel box_color = params[MGA_BOX_COLOR]->u.cd.value;
    double box_opacity = params[MGA_BOX_OPACITY]->u.fs_d.value;
    PF_Pixel outline_color = params[MGA_OUTLINE_COLOR]->u.cd.value;
    double outline_opacity = params[MGA_OUTLINE_OPACITY]->u.fs_d.value;
    double outline_width = params[MGA_OUTLINE_WIDTH]->u.fs_d.value;
    double border_radius = params[MGA_BORDER_RADIUS]->u.fs_d.value;
    double pad_left = params[MGA_PADDING_LEFT]->u.fs_d.value;
    double pad_right = params[MGA_PADDING_RIGHT]->u.fs_d.value;
    double pad_top = params[MGA_PADDING_TOP]->u.fs_d.value;
    double pad_bottom = params[MGA_PADDING_BOTTOM]->u.fs_d.value;
    bool uniform_scale = (params[MGA_UNIFORM_SCALE]->u.bd.value != 0);
    bool keep_aspect = (params[MGA_KEEP_ASPECT]->u.bd.value != 0);
    double scale_x_pct = params[MGA_SCALE_X]->u.fs_d.value;
    double scale_y_pct = params[MGA_SCALE_Y]->u.fs_d.value;
    if (uniform_scale) {
        scale_y_pct = scale_x_pct;
    }
    double offset_x = params[MGA_OFFSET_X]->u.fs_d.value;
    double offset_y = params[MGA_OFFSET_Y]->u.fs_d.value;
    double rotate_deg = (double)params[MGA_ROTATE]->u.ad.value / 65536.0;
    int comp_mode = params[MGA_COMP_MODE]->u.pd.value;
    double word_gap = params[MGA_WORD_GAP]->u.fs_d.value;
    double line_gap = params[MGA_LINE_GAP]->u.fs_d.value;
    int mb_enable = params[MGA_MB_ENABLE]->u.pd.value;
    int mb_samples = (int)params[MGA_MB_SAMPLES]->u.fs_d.value;
    bool fill_outline = (params[MGA_FILL_OUTLINE]->u.bd.value != 0);

    bool repeater_enable = (params[MGA_REPEATER_ENABLE]->u.bd.value != 0);
    int repeater_count_l = (int)params[MGA_REPEATER_COUNT_L]->u.fs_d.value;
    int repeater_count_r = (int)params[MGA_REPEATER_COUNT_R]->u.fs_d.value;
    int repeater_count_u = (int)params[MGA_REPEATER_COUNT_U]->u.fs_d.value;
    int repeater_count_d = (int)params[MGA_REPEATER_COUNT_D]->u.fs_d.value;
    double repeater_gap_x = params[MGA_REPEATER_GAP_X]->u.fs_d.value;
    double repeater_gap_y = params[MGA_REPEATER_GAP_Y]->u.fs_d.value;
    double repeater_offset_x = params[MGA_REPEATER_OFFSET_X]->u.fs_d.value;
    double repeater_offset_y = params[MGA_REPEATER_OFFSET_Y]->u.fs_d.value;
    bool repeater_show_text = (params[MGA_REPEATER_SHOW_TEXT]->u.bd.value != 0);
    bool repeater_show_shape = (params[MGA_REPEATER_SHOW_SHAPE]->u.bd.value != 0);
    double repeater_trans_x = params[MGA_REPEATER_TRANS_X]->u.fs_d.value;
    double repeater_trans_y = params[MGA_REPEATER_TRANS_Y]->u.fs_d.value;
    double repeater_trans_rotate = (double)params[MGA_REPEATER_TRANS_ROTATE]->u.ad.value / 65536.0;



    double scale_x = 1.0;
    if (in_data->downsample_x.den != 0) {
        scale_x = (double)in_data->downsample_x.num / in_data->downsample_x.den;
    }
    double scale_y = 1.0;
    if (in_data->downsample_y.den != 0) {
        scale_y = (double)in_data->downsample_y.num / in_data->downsample_y.den;
    }

    double render_offset_x = offset_x * scale_x;
    double render_offset_y = offset_y * scale_y;
    double render_pad_left = pad_left * scale_x;
    double render_pad_right = pad_right * scale_x;
    double render_pad_top = pad_top * scale_y;
    double render_pad_bottom = pad_bottom * scale_y;
    double render_repeater_gap_x = repeater_gap_x * scale_x;
    double render_repeater_gap_y = repeater_gap_y * scale_y;
    double render_repeater_offset_x = repeater_offset_x * scale_x;
    double render_repeater_offset_y = repeater_offset_y * scale_y;
    double render_repeater_trans_x = repeater_trans_x * scale_x;
    double render_repeater_trans_y = repeater_trans_y * scale_y;
    double trans_rad = repeater_trans_rotate * (3.14159265358979323846 / 180.0);
    double trans_cos = cos(trans_rad);
    double trans_sin = sin(trans_rad);
    double render_stroke_width = outline_width * ((scale_x + scale_y) / 2.0);
    double render_border_radius = border_radius * ((scale_x + scale_y) / 2.0);

    bool do_mb = false;
    int effective_samples = 1;
    double shutter_angle_deg = 180.0;
    double shutter_phase_deg = -90.0;

    if (suites.PFInterfaceSuite1()) {
        suites.PFInterfaceSuite1()->AEGP_GetEffectLayer(in_data->effect_ref, &layerH);
    }

    GetMotionBlurSettings(in_data, suites, layerH, mb_enable, mb_samples, do_mb, effective_samples, shutter_angle_deg, shutter_phase_deg);

    std::vector<A_long> sample_times;
    if (!do_mb || effective_samples <= 1) {
        sample_times.push_back(in_data->current_time);
    }
    else {
        double d_shutter = (double)in_data->time_step * (shutter_angle_deg / 360.0);
        double t_start = (double)in_data->current_time +
            (double)in_data->time_step * (shutter_phase_deg / 360.0);
        for (int i = 0; i < effective_samples; ++i) {
            double frac = (double)i / (effective_samples - 1);
            double t = t_start + d_shutter * frac;
            A_long t_i = (A_long)round(t);
            // Clamp to comp time 0: at the very first frame(s), shutter samples
            // computed from a negative phase can land BEFORE the comp/layer
            // starts. A layer has no content at negative time, so those samples
            // returned an empty box list, which diluted (and at frame 0, could
            // fully zero out) the averaged outline alpha -- making the outline
            // vanish entirely on frame 0 and reappear from frame 1 onward.
            if (t_i < 0) {
                t_i = 0;
            }
            sample_times.push_back(t_i);
        }
    }

    RenderRefcon rc;
    rc.mode = mode;
    rc.target = target;
    rc.consistent_size = consistent_size;
    rc.box_color = box_color;
    rc.box_opacity = box_opacity;
    rc.outline_color = outline_color;
    rc.outline_opacity = outline_opacity;
    rc.render_stroke_width = render_stroke_width;
    rc.render_border_radius = render_border_radius;
    rc.render_pad_left = render_pad_left;
    rc.render_pad_right = render_pad_right;
    rc.render_pad_top = render_pad_top;
    rc.render_pad_bottom = render_pad_bottom;
    rc.scale_x_pct = scale_x_pct;
    rc.scale_y_pct = scale_y_pct;
    rc.render_offset_x = render_offset_x;
    rc.render_offset_y = render_offset_y;
    rc.theta = rotate_deg * (3.14159265358979323846 / 180.0);
    rc.cos_theta = cos(-rc.theta);
    rc.sin_theta = sin(-rc.theta);
    rc.has_layer_transform = has_layer_transform;
    rc.layer_cx = has_layer_transform ? layer_cx : 0.0;
    rc.layer_cy = has_layer_transform ? layer_cy : 0.0;
    rc.total_theta = has_layer_transform ? layer_rotation_rad : 0.0;
    rc.cos_total_theta = cos(-rc.total_theta);
    rc.sin_total_theta = sin(-rc.total_theta);
    rc.keep_aspect = keep_aspect;
    rc.comp_mode = comp_mode;
    rc.fill_outline = fill_outline;
    rc.input = &params[MGA_INPUT]->u.ld;
    rc.output = output;

    rc.repeater_enable = repeater_enable;
    rc.repeater_count_l = repeater_count_l;
    rc.repeater_count_r = repeater_count_r;
    rc.repeater_count_u = repeater_count_u;
    rc.repeater_count_d = repeater_count_d;
    rc.repeater_gap_x = render_repeater_gap_x;
    rc.repeater_gap_y = render_repeater_gap_y;
    rc.repeater_offset_x = render_repeater_offset_x;
    rc.repeater_offset_y = render_repeater_offset_y;
    rc.repeater_show_text = repeater_show_text;
    rc.repeater_show_shape = repeater_show_shape;
    rc.repeater_trans_x = render_repeater_trans_x;
    rc.repeater_trans_y = render_repeater_trans_y;
    rc.repeater_trans_cos = trans_cos;
    rc.repeater_trans_sin = trans_sin;

    rc.num_samples = (int)sample_times.size();
    rc.samples.resize(rc.num_samples);

    std::vector<PF_ParamDef> input_checkouts(rc.num_samples);
    std::vector<PF_ParamDef> custom_checkouts(rc.num_samples);
    std::vector<bool> input_checkout_success(rc.num_samples, false);
    std::vector<bool> custom_checkout_success(rc.num_samples, false);
    for (int i = 0; i < rc.num_samples; ++i) {
        AEFX_CLR_STRUCT(input_checkouts[i]);
        AEFX_CLR_STRUCT(custom_checkouts[i]);
    }

    std::vector<PF_EffectWorld> temp_worlds;

    std::vector<BBox> local_boxes;
    PF_EffectWorld main_temp_world;
    AEFX_CLR_STRUCT(main_temp_world);
    bool has_main_temp_world = false;

    double main_rotation = layer_rotation_rad;
    double main_anim_rot = 0.0;
    if (is_text && layerH) {
        A_Time t = { in_data->current_time, in_data->time_scale };
        A_Time comp_time = t;
        if (suites.LayerSuite8()) {
            suites.LayerSuite8()->AEGP_ConvertLayerToCompTime(layerH, &t, &comp_time);
        }
        main_anim_rot = GetTextLayerAnimatorRotation(suites, layerH, &comp_time);
    }
    double main_total_rotation = main_rotation + main_anim_rot;

    PF_EffectWorld* input_world = &params[MGA_INPUT]->u.ld;

    bool got_vector_boxes = false;
    if (is_text && layerH && target == MGA_TARGET_CHAR) {
        A_Time t = { in_data->current_time, in_data->time_scale };
        A_Time comp_time = t;
        if (suites.LayerSuite8()) {
            suites.LayerSuite8()->AEGP_ConvertLayerToCompTime(layerH, &t, &comp_time);
        }
        got_vector_boxes = GetTextVectorBoxes(in_data, suites, layerH, &comp_time, input_world, local_boxes);
    }

    if (!got_vector_boxes) {
        const bool needs_unrotate =
            has_layer_transform && abs(main_total_rotation) > 1.0e-7;
        if (needs_unrotate) {
            A_Err new_world_err = in_data->utils->new_world(in_data->effect_ref, input_world->width, input_world->height, PF_NewWorldFlag_NONE, &main_temp_world);
            if (new_world_err == PF_Err_NONE) {
                UnrotateInputWorld(in_data, input_world, main_total_rotation, layer_cx, layer_cy, &main_temp_world);
                local_boxes = FindTargetBoxes(in_data, &main_temp_world, target, word_gap, line_gap);
                has_main_temp_world = true;
                temp_worlds.push_back(main_temp_world);
            }
        }
        else {
            local_boxes = FindTargetBoxes(in_data, input_world, target, word_gap, line_gap);
        }
    }

    for (int i = 0; i < rc.num_samples; ++i) {
        A_long t_i = sample_times[i];

        A_Matrix4 sample_matrix = layer_matrix;
        if (has_layer_transform && t_i != in_data->current_time && layerH) {
            A_Time t = { t_i, in_data->time_scale };
            A_Time comp_time = t;
            if (suites.LayerSuite8()) {
                suites.LayerSuite8()->AEGP_ConvertLayerToCompTime(layerH, &t, &comp_time);
            }
            suites.LayerSuite8()->AEGP_GetLayerToWorldXform(layerH, &comp_time, &sample_matrix);
        }

        rc.samples[i].has_layer_transform = has_layer_transform;
        if (has_layer_transform) {
            double sample_rotation = 0.0;
            double render_cx = 0.0;
            double render_cy = 0.0;
            double anim_rot = 0.0;

            if (fd && i < (int)fd->samples.size()) {
                sample_rotation = fd->samples[i].layer_rotation_rad;
                render_cx = fd->samples[i].layer_cx;
                render_cy = fd->samples[i].layer_cy;
                anim_rot = fd->samples[i].animator_rotation_rad;
            }
            else {
                sample_rotation = layer_rotation_rad;
                render_cx = layer_cx;
                render_cy = layer_cy;
                anim_rot = 0.0;
            }

            rc.samples[i].layer_cx = render_cx;
            rc.samples[i].layer_cy = render_cy;
            rc.samples[i].cos_total_theta = cos(-sample_rotation);
            rc.samples[i].sin_total_theta = sin(-sample_rotation);
            rc.samples[i].animator_rotation = anim_rot;
            rc.samples[i].sample_theta = rc.theta + anim_rot;
            rc.samples[i].cos_sample_theta = cos(-rc.samples[i].sample_theta);
            rc.samples[i].sin_sample_theta = sin(-rc.samples[i].sample_theta);

            rc.samples[i].boxes = local_boxes;
            double delta_x = render_cx - layer_cx;
            double delta_y = render_cy - layer_cy;
            for (auto& box : rc.samples[i].boxes) {
                box.min_x += delta_x;
                box.max_x += delta_x;
                box.min_y += delta_y;
                box.max_y += delta_y;
            }
            if (has_main_temp_world) {
                rc.samples[i].input_world = main_temp_world;
            }
            else {
                rc.samples[i].input_world = *input_world;
            }
        }
        else {
            rc.samples[i].layer_cx = 0.0;
            rc.samples[i].layer_cy = 0.0;
            rc.samples[i].cos_total_theta = 1.0;
            rc.samples[i].sin_total_theta = 0.0;
            rc.samples[i].animator_rotation = 0.0;
            rc.samples[i].sample_theta = rc.theta;
            rc.samples[i].cos_sample_theta = rc.cos_theta;
            rc.samples[i].sin_sample_theta = rc.sin_theta;

            rc.samples[i].boxes = local_boxes;
            rc.samples[i].input_world = *input_world;
        }

        rc.samples[i].has_custom_layer = false;
        if (mode == MGA_MODE_CUSTOM_LAYER) {
            PF_Err custom_err =
                PF_CHECKOUT_PARAM(in_data, MGA_CUSTOM_LAYER, t_i, in_data->time_step,
                    in_data->time_scale, &custom_checkouts[i]);
            if (custom_err == PF_Err_NONE) {
                custom_checkout_success[i] = true;
                if (custom_checkouts[i].u.ld.data != nullptr) {
                    rc.samples[i].has_custom_layer = true;
                    rc.samples[i].custom_world = custom_checkouts[i].u.ld;
                    rc.samples[i].custom_bounds = GetWorldContentBounds(
                        &rc.samples[i].custom_world,
                        PF_WORLD_IS_DEEP(&rc.samples[i].custom_world) != 0);
                }
            }
        }
    }

    int num_valid_samples = 0;
    for (int i = 0; i < rc.num_samples; ++i) {
        if (!rc.samples[i].boxes.empty()) {
            num_valid_samples++;
        }
    }
    if (num_valid_samples == 0) {
        num_valid_samples = 1;
    }
    rc.num_valid_samples = num_valid_samples;

    double global_min_x = 999999.0, global_max_x = -999999.0;
    double global_min_y = 999999.0, global_max_y = -999999.0;
    double max_box_w = 0.0, max_box_h = 0.0;
    bool has_any_box = false;

    for (int i = 0; i < rc.num_samples; ++i) {
        for (const auto& box : rc.samples[i].boxes) {
            has_any_box = true;
            double w = box.max_x - box.min_x + 1;
            double h = box.max_y - box.min_y + 1;
            if (w > max_box_w)
                max_box_w = w;
            if (h > max_box_h)
                max_box_h = h;
            if (box.min_x < global_min_x)
                global_min_x = box.min_x;
            if (box.max_x > global_max_x)
                global_max_x = box.max_x;
            if (box.min_y < global_min_y)
                global_min_y = box.min_y;
            if (box.max_y > global_max_y)
                global_max_y = box.max_y;
        }
    }

    rc.global_cx = has_any_box ? (global_min_x + global_max_x) / 2.0 : 0.0;
    rc.global_cy = has_any_box ? (global_min_y + global_max_y) / 2.0 : 0.0;

    rc.max_char_w = max_box_w;
    rc.max_char_h = max_box_h;

    // OPTIMIZATION #1: precompute box geometry once per box, per sample --
    // must happen after rc.max_char_w / rc.max_char_h are known (since
    // "consistent size" mode depends on them), and before the pixel loop in
    // CallIterate() below, which previously recomputed this same math on
    // every single pixel.
    for (int i = 0; i < rc.num_samples; ++i) {
        PrecomputeBoxGeometry(rc.samples[i].boxes, &rc, rc.samples[i].cos_sample_theta, rc.samples[i].sin_sample_theta, rc.samples[i].precomputed);
        if (!rc.repeater_enable) {
            BuildRowBoxCandidates(rc.samples[i]);
        }
    }

    // Exact rotated bounding box calculation
    double exact_min_x = 999999.0, exact_max_x = -999999.0;
    double exact_min_y = 999999.0, exact_max_y = -999999.0;

    // Include original unshifted bounding boxes to ensure their pixels
    // are processed (and cleared) if shifted.
    for (int i = 0; i < rc.num_samples; ++i) {
        for (size_t bi = 0; bi < rc.samples[i].boxes.size(); ++bi) {
            const auto& pb = rc.samples[i].precomputed[bi];
            double corners_x[4] = { pb.box_cx - pb.bound_x, pb.box_cx + pb.bound_x, pb.box_cx - pb.bound_x, pb.box_cx + pb.bound_x };
            double corners_y[4] = { pb.box_cy - pb.bound_y, pb.box_cy - pb.bound_y, pb.box_cy + pb.bound_y, pb.box_cy + pb.bound_y };

            for (int c = 0; c < 4; ++c) {
                double xf = corners_x[c];
                double yf = corners_y[c];
                if (rc.has_layer_transform) {
                    double dx = corners_x[c] - rc.layer_cx;
                    double dy = corners_y[c] - rc.layer_cy;
                    double cos_l = cos(layer_rotation_rad);
                    double sin_l = sin(layer_rotation_rad);
                    xf = rc.layer_cx + dx * cos_l - dy * sin_l;
                    yf = rc.layer_cy + dx * sin_l + dy * cos_l;
                }
                if (xf < exact_min_x) exact_min_x = xf;
                if (xf > exact_max_x) exact_max_x = xf;
                if (yf < exact_min_y) exact_min_y = yf;
                if (yf > exact_max_y) exact_max_y = yf;
            }
        }
    }

    for (int i = 0; i < rc.num_samples; ++i) {
        for (size_t bi = 0; bi < rc.samples[i].boxes.size(); ++bi) {
            const auto& pb = rc.samples[i].precomputed[bi];

            int start_col = rc.repeater_enable ? -rc.repeater_count_l : 0;
            int end_col = rc.repeater_enable ? rc.repeater_count_r : 0;
            int start_row = rc.repeater_enable ? -rc.repeater_count_u : 0;
            int end_row = rc.repeater_enable ? rc.repeater_count_d : 0;

            for (int row = start_row; row <= end_row; ++row) {
                for (int col = start_col; col <= end_col; ++col) {
                    double cx_rep = pb.box_cx + col * (pb.box_w + rc.repeater_gap_x) + row * rc.repeater_offset_x;
                    double cy_rep = pb.box_cy + row * (pb.box_h + rc.repeater_gap_y) + col * rc.repeater_offset_y;

                    double corners_x[4] = { cx_rep - pb.bound_x, cx_rep + pb.bound_x, cx_rep - pb.bound_x, cx_rep + pb.bound_x };
                    double corners_y[4] = { cy_rep - pb.bound_y, cy_rep - pb.bound_y, cy_rep + pb.bound_y, cy_rep + pb.bound_y };

                    for (int c = 0; c < 4; ++c) {
                        double xf = corners_x[c];
                        double yf = corners_y[c];

                        if (rc.repeater_enable) {
                            double dx = corners_x[c] - rc.global_cx;
                            double dy = corners_y[c] - rc.global_cy;
                            xf = rc.global_cx + (dx * rc.repeater_trans_cos - dy * rc.repeater_trans_sin) + rc.repeater_trans_x;
                            yf = rc.global_cy + (dx * rc.repeater_trans_sin + dy * rc.repeater_trans_cos) + rc.repeater_trans_y;
                        }

                        if (rc.has_layer_transform) {
                            double dx = xf - rc.layer_cx;
                            double dy = yf - rc.layer_cy;
                            double cos_l = cos(layer_rotation_rad);
                            double sin_l = sin(layer_rotation_rad);
                            xf = rc.layer_cx + dx * cos_l - dy * sin_l;
                            yf = rc.layer_cy + dx * sin_l + dy * cos_l;
                        }

                        if (xf < exact_min_x) exact_min_x = xf;
                        if (xf > exact_max_x) exact_max_x = xf;
                        if (yf < exact_min_y) exact_min_y = yf;
                        if (yf > exact_max_y) exact_max_y = yf;
                    }
                }
            }
        }
    }

    if (!has_any_box) {
        rc.render_min_x = 0.0;
        rc.render_max_x = -1.0;
        rc.render_min_y = 0.0;
        rc.render_max_y = -1.0;
    }
    else {
        rc.render_min_x = exact_min_x - 5.0;
        rc.render_max_x = exact_max_x + 5.0;
        rc.render_min_y = exact_min_y - 5.0;
        rc.render_max_y = exact_max_y + 5.0;
    }

    A_long linesL = output->extent_hint.bottom - output->extent_hint.top;

    err = CallIterate(in_data, linesL, &params[MGA_INPUT]->u.ld, (void*)&rc,
        output, PF_WORLD_IS_DEEP(output) != 0);

    for (int i = 0; i < rc.num_samples; ++i) {
        if (input_checkout_success[i]) {
            PF_CHECKIN_PARAM(in_data, &input_checkouts[i]);
        }
        if (custom_checkout_success[i]) {
            PF_CHECKIN_PARAM(in_data, &custom_checkouts[i]);
        }
    }

    for (auto& tw : temp_worlds) {
        in_data->utils->dispose_world(in_data->effect_ref, &tw);
    }

    if (in_data->frame_data && suites.HandleSuite1()) {
        suites.HandleSuite1()->host_unlock_handle(in_data->frame_data);
    }

    return err;
}

static PF_Err UpdateParameterUI(PF_InData* in_data, PF_OutData* out_data,
    void* extra) {
    PF_Err err = PF_Err_NONE;
    if (s_my_aegp_id == 0)
        return err;

    AEGP_SuiteHandler suites(in_data->pica_basicP);
    AEGP_EffectRefH effectH = NULL;
    suites.PFInterfaceSuite1()->AEGP_GetNewEffectForEffect(
        s_my_aegp_id, in_data->effect_ref, &effectH);

    if (effectH) {
        auto GetParamValue = [&](int param_index) -> A_long {
            AEGP_StreamRefH streamH = NULL;
            A_long val = 0;
            if (!suites.StreamSuite5()->AEGP_GetNewEffectStreamByIndex(
                s_my_aegp_id, effectH, param_index, &streamH) &&
                streamH) {
                AEGP_StreamValue2 stream_val;
                AEFX_CLR_STRUCT(stream_val);
                A_Time t = { in_data->current_time, in_data->time_scale };
                if (!suites.StreamSuite5()->AEGP_GetNewStreamValue(
                    s_my_aegp_id, streamH, AEGP_LTimeMode_CompTime, &t, TRUE,
                    &stream_val)) {
                    val = (A_long)stream_val.val.one_d;
                    suites.StreamSuite5()->AEGP_DisposeStreamValue(&stream_val);
                }
                suites.StreamSuite5()->AEGP_DisposeStream(streamH);
            }
            return val;
            };

        auto SetParamHiddenAEGP = [&](int param_index, bool hide) {
            AEGP_StreamRefH streamH = NULL;
            if (!suites.StreamSuite5()->AEGP_GetNewEffectStreamByIndex(
                s_my_aegp_id, effectH, param_index, &streamH) &&
                streamH) {
                AEGP_DynStreamFlags flags = 0;
                suites.DynamicStreamSuite4()->AEGP_GetDynamicStreamFlags(streamH,
                    &flags);
                bool is_hidden = (flags & AEGP_DynStreamFlag_HIDDEN) != 0;
                if (is_hidden != hide) {
                    suites.DynamicStreamSuite4()->AEGP_SetDynamicStreamFlag(
                        streamH, AEGP_DynStreamFlag_HIDDEN, FALSE, hide);
                }
                suites.StreamSuite5()->AEGP_DisposeStream(streamH);
            }
            };

        int mode = GetParamValue(MGA_MODE);
        int target = GetParamValue(MGA_TARGET);
        int mb_enable = GetParamValue(MGA_MB_ENABLE);
        bool uniform_scale = (GetParamValue(MGA_UNIFORM_SCALE) != 0);

        bool is_custom = (mode == MGA_MODE_CUSTOM_LAYER);
        bool show_outline =
            (mode == MGA_MODE_OUTLINE || mode == MGA_MODE_BOTH ||
                mode == MGA_MODE_TEXT_OUTLINE || mode == MGA_MODE_TEXT_BOTH);
        bool show_solid =
            (mode == MGA_MODE_SOLID || mode == MGA_MODE_BOTH ||
                mode == MGA_MODE_TEXT_SOLID || mode == MGA_MODE_TEXT_BOTH);
        bool hide_radius = is_custom || (mode == MGA_MODE_TEXT_OUTLINE ||
            mode == MGA_MODE_TEXT_SOLID ||
            mode == MGA_MODE_TEXT_BOTH);

        // Hide/show size mode based on target (only relevant for Per Word and Per
        // Character)
        SetParamHiddenAEGP(MGA_SIZE_MODE, (target == 1));

        // Hide/show styling parameters
        SetParamHiddenAEGP(MGA_BOX_COLOR, !show_solid);
        SetParamHiddenAEGP(MGA_BOX_OPACITY, !show_solid);
        SetParamHiddenAEGP(MGA_OUTLINE_COLOR, !show_outline);
        SetParamHiddenAEGP(MGA_OUTLINE_OPACITY, !show_outline);
        SetParamHiddenAEGP(MGA_OUTLINE_WIDTH, !show_outline);
        SetParamHiddenAEGP(MGA_BORDER_RADIUS, hide_radius);

        // Hide/show custom layer parameters
        SetParamHiddenAEGP(MGA_CUSTOM_LAYER, !is_custom);
        SetParamHiddenAEGP(MGA_KEEP_ASPECT, !is_custom);

        // Uniform scale hiding
        SetParamHiddenAEGP(MGA_SCALE_Y, uniform_scale);

        // Motion blur samples hiding
        SetParamHiddenAEGP(MGA_MB_SAMPLES, (mb_enable != 3));

        // Fill outline hiding (only show in Text Outline mode)
        SetParamHiddenAEGP(MGA_FILL_OUTLINE, mode != MGA_MODE_TEXT_OUTLINE);

        bool repeater_enabled = (GetParamValue(MGA_REPEATER_ENABLE) != 0);
        SetParamHiddenAEGP(MGA_REPEATER_COUNT_L, !repeater_enabled);
        SetParamHiddenAEGP(MGA_REPEATER_COUNT_R, !repeater_enabled);
        SetParamHiddenAEGP(MGA_REPEATER_COUNT_U, !repeater_enabled);
        SetParamHiddenAEGP(MGA_REPEATER_COUNT_D, !repeater_enabled);
        SetParamHiddenAEGP(MGA_REPEATER_GAP_X, !repeater_enabled);
        SetParamHiddenAEGP(MGA_REPEATER_GAP_Y, !repeater_enabled);
        SetParamHiddenAEGP(MGA_REPEATER_OFFSET_X, !repeater_enabled);
        SetParamHiddenAEGP(MGA_REPEATER_OFFSET_Y, !repeater_enabled);
        SetParamHiddenAEGP(MGA_REPEATER_SHOW_TEXT, !repeater_enabled);
        SetParamHiddenAEGP(MGA_REPEATER_SHOW_SHAPE, !repeater_enabled);
        SetParamHiddenAEGP(MGA_REPEATER_TRANS_X, !repeater_enabled);
        SetParamHiddenAEGP(MGA_REPEATER_TRANS_Y, !repeater_enabled);
        SetParamHiddenAEGP(MGA_REPEATER_TRANS_ROTATE, !repeater_enabled);

        suites.EffectSuite4()->AEGP_DisposeEffect(effectH);
    }
    return err;
}

extern "C" DllExport PF_Err PluginDataEntryFunction2(
    PF_PluginDataPtr inPtr, PF_PluginDataCB2 inPluginDataCallBackPtr,
    SPBasicSuite* inSPBasicSuitePtr, const char* inHostName,
    const char* inHostVersion) {
    PF_Err result = PF_Err_INVALID_CALLBACK;

    result = PF_REGISTER_EFFECT_EXT2(inPtr, inPluginDataCallBackPtr,
        "mgAround",               // Name
        "ADBE mgAround",          // Match Name
        "Sample Plug-ins",        // Category
        AE_RESERVED_INFO,         // Reserved Info
        "EffectMain",             // Entry point
        "https://www.adobe.com"); // support URL

    return result;
}

PF_Err EffectMain(PF_Cmd cmd, PF_InData* in_data, PF_OutData* out_data,
    PF_ParamDef* params[], PF_LayerDef* output, void* extra) {
    PF_Err err = PF_Err_NONE;

    try {
        switch (cmd) {
        case PF_Cmd_ABOUT:
            err = About(in_data, out_data, params, output);
            break;

        case PF_Cmd_GLOBAL_SETUP:
            err = GlobalSetup(in_data, out_data, params, output);
            break;

        case PF_Cmd_PARAMS_SETUP:
            err = ParamsSetup(in_data, out_data, params, output);
            break;

        case PF_Cmd_FRAME_SETUP:
            err = FrameSetup(in_data, out_data, params, output);
            break;

        case PF_Cmd_FRAME_SETDOWN:
            err = FrameSetdown(in_data, out_data, params, output);
            break;

        case PF_Cmd_RENDER:
            err = Render(in_data, out_data, params, output);
            break;

        case PF_Cmd_UPDATE_PARAMS_UI:
            err = UpdateParameterUI(in_data, out_data, extra);
            break;
        }
    }
    catch (PF_Err& thrown_err) {
        err = thrown_err;
    }
    return err;
}
