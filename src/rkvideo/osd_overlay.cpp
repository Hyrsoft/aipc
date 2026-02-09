/**
 * @file osd_overlay.cpp
 * @brief OSD 覆盖模块实现
 *
 * @author 好软，好温暖
 * @date 2026-02-10
 */

#define LOG_TAG "OSD"

#include "osd_overlay.h"
#include "common/logger.h"

#include <cstring>
#include <algorithm>

// Rockchip MPI headers
extern "C" {
#include "rk_mpi_rgn.h"
}

// ============================================================================
// OSDOverlay 实现
// ============================================================================

bool OSDOverlay::Init(int vencChn, const OSDConfig& config) {
    if (initialized_) {
        LOG_WARN("OSD already initialized");
        return true;
    }
    
    venc_chn_ = vencChn;
    config_ = config;
    
    // 预分配 bitmap 缓冲区（corner_size x corner_size ARGB8888）
    int bufferSize = config_.corner_size * config_.corner_size;
    bitmap_buffer_.resize(bufferSize);
    
    // 计算最大 RGN 数量：每个框 4 个角，最多 max_boxes 个框
    int maxHandles = config_.max_boxes * 4;
    active_handles_.reserve(maxHandles);
    
    initialized_ = true;
    LOG_INFO("OSD initialized: venc_chn={}, max_boxes={}, corner_size={}", 
             vencChn, config_.max_boxes, config_.corner_size);
    
    return true;
}

void OSDOverlay::Deinit() {
    if (!initialized_) {
        return;
    }
    
    DestroyAllRGN();
    initialized_ = false;
    
    LOG_INFO("OSD deinitialized");
}

void OSDOverlay::UpdateBoxes(const std::vector<OSDBox>& boxes) {
    if (!initialized_) {
        return;
    }
    
    // 先清除旧的 RGN
    DestroyAllRGN();
    
    // 限制最大框数
    size_t numBoxes = std::min(boxes.size(), static_cast<size_t>(config_.max_boxes));
    
    if (numBoxes == 0) {
        return;
    }
    
    // 为每个框创建 4 个角标记
    for (size_t i = 0; i < numBoxes; ++i) {
        const auto& box = boxes[i];
        uint32_t color = box.color ? box.color : config_.default_color;
        
        // RGN 要求坐标必须是 2 的倍数（2像素对齐）
        // 对坐标进行对齐处理
        int aligned_x = box.x & ~1;  // 向下对齐到偶数
        int aligned_y = box.y & ~1;
        int aligned_right = (box.x + box.width) & ~1;
        int aligned_bottom = (box.y + box.height) & ~1;
        
        // 确保 corner_size 也是偶数
        int corner_size = config_.corner_size & ~1;
        if (corner_size < 4) corner_size = 4;
        
        // 四个角的位置
        // 0: 左上, 1: 右上, 2: 左下, 3: 右下
        struct {
            int x, y, type;
        } corners[4] = {
            { aligned_x, aligned_y, 0 },                                   // 左上
            { aligned_right - corner_size, aligned_y, 1 },                 // 右上
            { aligned_x, aligned_bottom - corner_size, 2 },                // 左下
            { aligned_right - corner_size, aligned_bottom - corner_size, 3 } // 右下
        };
        
        for (int c = 0; c < 4; ++c) {
            // 再次确保坐标对齐（处理边界情况）
            int pos_x = corners[c].x & ~1;
            int pos_y = corners[c].y & ~1;
            
            // 确保坐标非负
            if (pos_x < 0) pos_x = 0;
            if (pos_y < 0) pos_y = 0;
            
            int handle = next_handle_++;
            
            // 填充角标记 bitmap（使用对齐后的尺寸）
            FillCornerBitmap(bitmap_buffer_.data(), corner_size, corners[c].type, color);
            
            // 创建并配置 RGN（使用对齐后的坐标和尺寸）
            if (CreateCornerRGN(handle, pos_x, pos_y, corner_size)) {
                // 设置 bitmap
                BITMAP_S stBitmap;
                memset(&stBitmap, 0, sizeof(stBitmap));
                stBitmap.enPixelFormat = RK_FMT_ARGB8888;
                stBitmap.u32Width = corner_size;
                stBitmap.u32Height = corner_size;
                stBitmap.pData = bitmap_buffer_.data();
                
                RK_S32 ret = RK_MPI_RGN_SetBitMap(handle, &stBitmap);
                if (ret != RK_SUCCESS) {
                    LOG_WARN("RK_MPI_RGN_SetBitMap failed: handle={}, ret={:#x}", handle, ret);
                }
                
                active_handles_.push_back(handle);
            }
        }
    }
    
    LOG_DEBUG("OSD updated: {} boxes, {} RGN handles", numBoxes, active_handles_.size());
}

void OSDOverlay::Clear() {
    if (!initialized_) {
        return;
    }
    
    DestroyAllRGN();
    LOG_DEBUG("OSD cleared");
}

bool OSDOverlay::CreateCornerRGN(int handle, int x, int y, int size) {
    // 创建 OVERLAY 类型的 RGN
    RGN_ATTR_S stRgnAttr;
    memset(&stRgnAttr, 0, sizeof(stRgnAttr));
    stRgnAttr.enType = OVERLAY_RGN;
    stRgnAttr.unAttr.stOverlay.enPixelFmt = RK_FMT_ARGB8888;
    stRgnAttr.unAttr.stOverlay.stSize.u32Width = size;
    stRgnAttr.unAttr.stOverlay.stSize.u32Height = size;
    
    RK_S32 ret = RK_MPI_RGN_Create(handle, &stRgnAttr);
    if (ret != RK_SUCCESS) {
        LOG_WARN("RK_MPI_RGN_Create failed: handle={}, ret={:#x}", handle, ret);
        return false;
    }
    
    // 附加到 VENC 通道
    MPP_CHN_S stMppChn;
    memset(&stMppChn, 0, sizeof(stMppChn));
    stMppChn.enModId = RK_ID_VENC;
    stMppChn.s32DevId = 0;
    stMppChn.s32ChnId = venc_chn_;
    
    RGN_CHN_ATTR_S stChnAttr;
    memset(&stChnAttr, 0, sizeof(stChnAttr));
    stChnAttr.bShow = RK_TRUE;
    stChnAttr.enType = OVERLAY_RGN;
    stChnAttr.unChnAttr.stOverlayChn.stPoint.s32X = x;
    stChnAttr.unChnAttr.stOverlayChn.stPoint.s32Y = y;
    stChnAttr.unChnAttr.stOverlayChn.u32BgAlpha = 0;      // 完全透明背景
    stChnAttr.unChnAttr.stOverlayChn.u32FgAlpha = 255;    // 不透明前景
    stChnAttr.unChnAttr.stOverlayChn.u32Layer = 0;
    
    ret = RK_MPI_RGN_AttachToChn(handle, &stMppChn, &stChnAttr);
    if (ret != RK_SUCCESS) {
        LOG_WARN("RK_MPI_RGN_AttachToChn failed: handle={}, ret={:#x}", handle, ret);
        RK_MPI_RGN_Destroy(handle);
        return false;
    }
    
    return true;
}

void OSDOverlay::DestroyAllRGN() {
    MPP_CHN_S stMppChn;
    memset(&stMppChn, 0, sizeof(stMppChn));
    stMppChn.enModId = RK_ID_VENC;
    stMppChn.s32DevId = 0;
    stMppChn.s32ChnId = venc_chn_;
    
    for (int handle : active_handles_) {
        RK_MPI_RGN_DetachFromChn(handle, &stMppChn);
        RK_MPI_RGN_Destroy(handle);
    }
    
    active_handles_.clear();
}

void OSDOverlay::FillCornerBitmap(uint32_t* buffer, int size, int cornerType, uint32_t color) {
    int lineWidth = config_.line_width;
    
    // 清空为透明
    memset(buffer, 0, size * size * sizeof(uint32_t));
    
    // 根据角类型绘制 L 形标记
    // cornerType: 0=左上, 1=右上, 2=左下, 3=右下
    for (int y = 0; y < size; ++y) {
        for (int x = 0; x < size; ++x) {
            bool draw = false;
            
            switch (cornerType) {
                case 0:  // 左上：左边和上边
                    draw = (x < lineWidth) || (y < lineWidth);
                    break;
                case 1:  // 右上：右边和上边
                    draw = (x >= size - lineWidth) || (y < lineWidth);
                    break;
                case 2:  // 左下：左边和下边
                    draw = (x < lineWidth) || (y >= size - lineWidth);
                    break;
                case 3:  // 右下：右边和下边
                    draw = (x >= size - lineWidth) || (y >= size - lineWidth);
                    break;
            }
            
            if (draw) {
                buffer[y * size + x] = color;
            }
        }
    }
}

// ============================================================================
// 全局实例和便捷函数
// ============================================================================

static OSDOverlay g_osd;

bool osd_init(int vencChn, const OSDConfig& config) {
    return g_osd.Init(vencChn, config);
}

void osd_deinit() {
    g_osd.Deinit();
}

void osd_update_boxes(const std::vector<OSDBox>& boxes) {
    g_osd.UpdateBoxes(boxes);
}

void osd_clear() {
    g_osd.Clear();
}
