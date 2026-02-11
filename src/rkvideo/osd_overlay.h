/**
 * @file osd_overlay.h
 * @brief OSD 覆盖模块 - 在编码视频流上绘制检测框
 *
 * 基于 Rockchip RGN (Region) 模块实现，将检测结果叠加到 VENC 输出。
 * 
 * 架构：
 *   VPSS Chn0 --> VENC --> 编码流
 *                  ^
 *                  |
 *                 RGN (OVERLAY_RGN) 
 *                  |
 *                  +--> 检测框/标签
 *
 * @author 好软，好温暖
 * @date 2026-02-10
 */

#pragma once

#include <vector>
#include <cstdint>

// ============================================================================
// 检测框结构
// ============================================================================

struct OSDBox {
    int x;          // 左上角 X
    int y;          // 左上角 Y
    int width;      // 宽度
    int height;     // 高度
    uint32_t color; // ARGB8888 颜色 (如 0xFF0000FF = 红色不透明)
    int label_id;   // 类别 ID（可选，用于显示标签）
};

// ============================================================================
// OSD 配置
// ============================================================================

struct OSDConfig {
    int max_boxes = 8;          // 最大同时显示的检测框数量
    int line_width = 4;         // 边框线宽（像素）
    int corner_size = 20;       // 角标记大小（像素）
    bool show_corners_only = true;  // 仅显示角标记（性能更好）
    uint32_t default_color = 0x00FF00FF;  // 默认颜色：绿色不透明
};

// ============================================================================
// OSD Overlay 类
// ============================================================================

class OSDOverlay {
public:
    /**
     * @brief 初始化 OSD 模块
     * 
     * @param vencChn 要附加 OSD 的 VENC 通道号
     * @param config OSD 配置
     * @return true 成功
     */
    bool Init(int vencChn, const OSDConfig& config = OSDConfig());
    
    /**
     * @brief 反初始化 OSD 模块
     */
    void Deinit();
    
    /**
     * @brief 更新检测框显示
     * 
     * 清除旧的检测框，显示新的检测框列表
     * 
     * @param boxes 检测框列表
     */
    void UpdateBoxes(const std::vector<OSDBox>& boxes);
    
    /**
     * @brief 清除所有检测框
     */
    void Clear();
    
    /**
     * @brief 检查是否已初始化
     */
    bool IsInitialized() const { return initialized_; }

private:
    // 创建一个角标记 RGN
    bool CreateCornerRGN(int handle, int x, int y, int size);
    
    // 销毁所有 RGN
    void DestroyAllRGN();
    
    // 填充角标记 bitmap
    void FillCornerBitmap(uint32_t* buffer, int size, int cornerType, uint32_t color);

private:
    bool initialized_ = false;
    int venc_chn_ = 0;
    OSDConfig config_;
    
    // 当前活动的 RGN handles
    // 每个检测框使用 4 个 RGN（四个角）
    std::vector<int> active_handles_;
    int next_handle_ = 0;
    
    // 预分配的 bitmap 缓冲区
    std::vector<uint32_t> bitmap_buffer_;
};

// ============================================================================
// 全局便捷函数
// ============================================================================

/**
 * @brief 初始化全局 OSD 实例
 */
bool osd_init(int vencChn, const OSDConfig& config = OSDConfig());

/**
 * @brief 反初始化全局 OSD 实例
 */
void osd_deinit();

/**
 * @brief 更新检测框显示
 */
void osd_update_boxes(const std::vector<OSDBox>& boxes);

/**
 * @brief 清除所有检测框
 */
void osd_clear();
