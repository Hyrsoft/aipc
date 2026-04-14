/**
 * @file simple_ipc_config.h
 * @brief SimpleIPC 模式专用配置
 *
 * 包含分辨率枚举与配置结构体，仅对 SimpleIPC 模式有效。
 *
 * 设计原则：
 * - SimpleIPC 通过 RKMPI（VI/VPSS/VENC）进行硬件级分辨率切换，
 *   分辨率是一个有限的、明确的枚举选项。
 * - VisionG 模式由 Python 脚本通过 visiong.Camera(w, h, ...) 自行决定
 *   摄像头分辨率，C++ 层无需感知，因此这套配置不属于共有接口。
 *
 * @author 好软，好温暖
 * @date 2026-02-12
 */

#pragma once

namespace media {

    // ============================================================================
    // 前向声明（避免循环包含）
    // ============================================================================
    struct ProducerConfig;

    namespace simple_ipc {

        // ========================================================================
        // 分辨率预设枚举
        // ========================================================================

        /**
         * @brief SimpleIPC 模式分辨率预设
         *
         * 对应 RKMPI 硬件支持的几个常见档位。
         * 切换分辨率需要重新初始化 VI/VPSS/VENC 管线（冷切换）。
         */
        enum class Resolution {
            R_1080P, ///< 1920x1080 @ 30fps（默认，高清监控）
            R_720P, ///< 1280x720  @ 30fps
            R_480P, ///< 720x480   @ 30fps
        };

        // ========================================================================
        // 分辨率配置结构体
        // ========================================================================

        /**
         * @brief 分辨率参数（宽、高、帧率）
         *
         * 通常由 ResolutionConfig::FromPreset() 从枚举值生成，
         * 再传递给 RKMPI 初始化函数。
         */
        struct ResolutionConfig {
            int width = 1920;
            int height = 1080;
            int framerate = 30;

            /**
             * @brief 从预设枚举生成分辨率配置
             */
            static ResolutionConfig FromPreset(Resolution preset) {
                switch (preset) {
                    case Resolution::R_1080P:
                        return {1920, 1080, 30};
                    case Resolution::R_720P:
                        return {1280, 720, 30};
                    case Resolution::R_480P:
                        return {720, 480, 30};
                    default:
                        return {1920, 1080, 30};
                }
            }
        };

    } // namespace simple_ipc

    // ============================================================================
    // SimpleIPCConfig — 在 ProducerConfig 基础上增加 SimpleIPC 专有字段
    // ============================================================================

    /**
     * @brief SimpleIPC 模式完整配置
     *
     * 继承自 ProducerConfig（包含 framerate、bitrate_kbps 等共有参数），
     * 并追加 SimpleIPC 专属的分辨率预设字段。
     *
     * 使用方式：
     * @code
     * media::SimpleIPCConfig cfg;
     * cfg.resolution    = media::simple_ipc::Resolution::R_1080P;
     * cfg.framerate     = 30;
     * cfg.bitrate_kbps  = 10 * 1024;
     * auto producer = CreateSimpleIPCProducer(cfg);
     * @endcode
     */
    struct SimpleIPCConfig {
        // ── 共有参数（与 ProducerConfig 保持一致） ──────────────────────────
        int framerate = 30;
        int bitrate_kbps = 10 * 1024; // 10 Mbps

        // ── SimpleIPC 专有参数 ──────────────────────────────────────────────
        simple_ipc::Resolution resolution = simple_ipc::Resolution::R_1080P;

        /**
         * @brief 获取当前分辨率的具体参数（宽高帧率）
         *
         * framerate 字段会覆盖预设值中的默认帧率，
         * 以允许用户单独设置帧率而不影响分辨率预设。
         */
        simple_ipc::ResolutionConfig GetResolutionConfig() const {
            auto cfg = simple_ipc::ResolutionConfig::FromPreset(resolution);
            cfg.framerate = framerate;
            return cfg;
        }
    };

} // namespace media
