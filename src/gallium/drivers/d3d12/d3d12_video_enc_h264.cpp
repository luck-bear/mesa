/*
 * Copyright © Microsoft Corporation
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 * IN THE SOFTWARE.
 */

#include "d3d12_video_enc.h"
#include "d3d12_video_enc_h264.h"
#include "util/u_video.h"
#include "d3d12_screen.h"
#include "d3d12_format.h"

void
d3d12_video_encoder_update_current_rate_control_h264(struct d3d12_video_encoder *pD3D12Enc,
                                                     pipe_h264_enc_picture_desc *picture)
{
   auto previousConfig = pD3D12Enc->m_currentEncodeConfig.m_encoderRateControlDesc;

   pD3D12Enc->m_currentEncodeConfig.m_encoderRateControlDesc = {};
   pD3D12Enc->m_currentEncodeConfig.m_encoderRateControlDesc.m_FrameRate.Numerator =
      picture->rate_ctrl[0].frame_rate_num;
   pD3D12Enc->m_currentEncodeConfig.m_encoderRateControlDesc.m_FrameRate.Denominator =
      picture->rate_ctrl[0].frame_rate_den;
   pD3D12Enc->m_currentEncodeConfig.m_encoderRateControlDesc.m_Flags = D3D12_VIDEO_ENCODER_RATE_CONTROL_FLAG_NONE;

   switch (picture->rate_ctrl[0].rate_ctrl_method) {
      case PIPE_H2645_ENC_RATE_CONTROL_METHOD_VARIABLE_SKIP:
      case PIPE_H2645_ENC_RATE_CONTROL_METHOD_VARIABLE:
      {
         pD3D12Enc->m_currentEncodeConfig.m_encoderRateControlDesc.m_Mode = D3D12_VIDEO_ENCODER_RATE_CONTROL_MODE_VBR;
         pD3D12Enc->m_currentEncodeConfig.m_encoderRateControlDesc.m_Config.m_Configuration_VBR.TargetAvgBitRate =
            picture->rate_ctrl[0].target_bitrate;
         pD3D12Enc->m_currentEncodeConfig.m_encoderRateControlDesc.m_Config.m_Configuration_VBR.PeakBitRate =
            picture->rate_ctrl[0].peak_bitrate;
      } break;
      case PIPE_H2645_ENC_RATE_CONTROL_METHOD_CONSTANT_SKIP:
      case PIPE_H2645_ENC_RATE_CONTROL_METHOD_CONSTANT:
      {
         pD3D12Enc->m_currentEncodeConfig.m_encoderRateControlDesc.m_Mode = D3D12_VIDEO_ENCODER_RATE_CONTROL_MODE_CBR;
         pD3D12Enc->m_currentEncodeConfig.m_encoderRateControlDesc.m_Config.m_Configuration_CBR.TargetBitRate =
            picture->rate_ctrl[0].target_bitrate;

         /* For CBR mode, to guarantee bitrate of generated stream complies with
          * target bitrate (e.g. no over +/-10%), vbv_buffer_size should be same
          * as target bitrate. Controlled by OS env var D3D12_VIDEO_ENC_CBR_FORCE_VBV_EQUAL_BITRATE
          */
         if (D3D12_VIDEO_ENC_CBR_FORCE_VBV_EQUAL_BITRATE) {
            debug_printf("[d3d12_video_encoder_h264] d3d12_video_encoder_update_current_rate_control_h264 D3D12_VIDEO_ENC_CBR_FORCE_VBV_EQUAL_BITRATE environment variable is set, "
                       ", forcing VBV Size = Target Bitrate = %ld (bits)\n", pD3D12Enc->m_currentEncodeConfig.m_encoderRateControlDesc.m_Config.m_Configuration_CBR.TargetBitRate);
            pD3D12Enc->m_currentEncodeConfig.m_encoderRateControlDesc.m_Flags |=
               D3D12_VIDEO_ENCODER_RATE_CONTROL_FLAG_ENABLE_VBV_SIZES;
            pD3D12Enc->m_currentEncodeConfig.m_encoderRateControlDesc.m_Config.m_Configuration_CBR.VBVCapacity =
               pD3D12Enc->m_currentEncodeConfig.m_encoderRateControlDesc.m_Config.m_Configuration_CBR.TargetBitRate;
         }

      } break;
      case PIPE_H2645_ENC_RATE_CONTROL_METHOD_DISABLE:
      {
         pD3D12Enc->m_currentEncodeConfig.m_encoderRateControlDesc.m_Mode = D3D12_VIDEO_ENCODER_RATE_CONTROL_MODE_CQP;
         pD3D12Enc->m_currentEncodeConfig.m_encoderRateControlDesc.m_Config.m_Configuration_CQP
            .ConstantQP_FullIntracodedFrame = picture->quant_i_frames;
         pD3D12Enc->m_currentEncodeConfig.m_encoderRateControlDesc.m_Config.m_Configuration_CQP
            .ConstantQP_InterPredictedFrame_PrevRefOnly = picture->quant_p_frames;
         pD3D12Enc->m_currentEncodeConfig.m_encoderRateControlDesc.m_Config.m_Configuration_CQP
            .ConstantQP_InterPredictedFrame_BiDirectionalRef = picture->quant_b_frames;
      } break;
      default:
      {
         debug_printf("[d3d12_video_encoder_h264] d3d12_video_encoder_update_current_rate_control_h264 invalid RC "
                       "config, using default RC CQP mode\n");
         pD3D12Enc->m_currentEncodeConfig.m_encoderRateControlDesc.m_Mode = D3D12_VIDEO_ENCODER_RATE_CONTROL_MODE_CQP;
         pD3D12Enc->m_currentEncodeConfig.m_encoderRateControlDesc.m_Config.m_Configuration_CQP
            .ConstantQP_FullIntracodedFrame = 30;
         pD3D12Enc->m_currentEncodeConfig.m_encoderRateControlDesc.m_Config.m_Configuration_CQP
            .ConstantQP_InterPredictedFrame_PrevRefOnly = 30;
         pD3D12Enc->m_currentEncodeConfig.m_encoderRateControlDesc.m_Config.m_Configuration_CQP
            .ConstantQP_InterPredictedFrame_BiDirectionalRef = 30;
      } break;
   }

   if (memcmp(&previousConfig,
              &pD3D12Enc->m_currentEncodeConfig.m_encoderRateControlDesc,
              sizeof(pD3D12Enc->m_currentEncodeConfig.m_encoderRateControlDesc)) != 0) {
      pD3D12Enc->m_currentEncodeConfig.m_ConfigDirtyFlags |= d3d12_video_encoder_config_dirty_flag_rate_control;
   }
}

void
d3d12_video_encoder_update_current_frame_pic_params_info_h264(struct d3d12_video_encoder *pD3D12Enc,
                                                              struct pipe_video_buffer *srcTexture,
                                                              struct pipe_picture_desc *picture,
                                                              D3D12_VIDEO_ENCODER_PICTURE_CONTROL_CODEC_DATA &picParams,
                                                              bool &bUsedAsReference)
{
   struct pipe_h264_enc_picture_desc *h264Pic = (struct pipe_h264_enc_picture_desc *) picture;
   d3d12_video_bitstream_builder_h264 *pH264BitstreamBuilder =
      dynamic_cast<d3d12_video_bitstream_builder_h264 *>(pD3D12Enc->m_upBitstreamBuilder.get());
   assert(pH264BitstreamBuilder != nullptr);

   bUsedAsReference = !h264Pic->not_referenced;

   picParams.pH264PicData->pic_parameter_set_id = pH264BitstreamBuilder->get_active_pps_id();
   picParams.pH264PicData->idr_pic_id = h264Pic->idr_pic_id;
   picParams.pH264PicData->FrameType = d3d12_video_encoder_convert_frame_type(h264Pic->picture_type);
   picParams.pH264PicData->PictureOrderCountNumber = h264Pic->pic_order_cnt;
   picParams.pH264PicData->FrameDecodingOrderNumber = h264Pic->frame_num;

   picParams.pH264PicData->List0ReferenceFramesCount = 0;
   picParams.pH264PicData->pList0ReferenceFrames = nullptr;
   picParams.pH264PicData->List1ReferenceFramesCount = 0;
   picParams.pH264PicData->pList1ReferenceFrames = nullptr;

   if (picParams.pH264PicData->FrameType == D3D12_VIDEO_ENCODER_FRAME_TYPE_H264_P_FRAME) {
      picParams.pH264PicData->List0ReferenceFramesCount = h264Pic->num_ref_idx_l0_active_minus1 + 1;
      picParams.pH264PicData->pList0ReferenceFrames = h264Pic->ref_idx_l0_list;
   } else if (picParams.pH264PicData->FrameType == D3D12_VIDEO_ENCODER_FRAME_TYPE_H264_B_FRAME) {
      picParams.pH264PicData->List0ReferenceFramesCount = h264Pic->num_ref_idx_l0_active_minus1 + 1;
      picParams.pH264PicData->pList0ReferenceFrames = h264Pic->ref_idx_l0_list;
      picParams.pH264PicData->List1ReferenceFramesCount = h264Pic->num_ref_idx_l1_active_minus1 + 1;
      picParams.pH264PicData->pList1ReferenceFrames = h264Pic->ref_idx_l1_list;
   }
}

D3D12_VIDEO_ENCODER_FRAME_TYPE_H264
d3d12_video_encoder_convert_frame_type(enum pipe_h2645_enc_picture_type picType)
{
   switch (picType) {
      case PIPE_H2645_ENC_PICTURE_TYPE_P:
      {
         return D3D12_VIDEO_ENCODER_FRAME_TYPE_H264_P_FRAME;
      } break;
      case PIPE_H2645_ENC_PICTURE_TYPE_B:
      {
         return D3D12_VIDEO_ENCODER_FRAME_TYPE_H264_B_FRAME;
      } break;
      case PIPE_H2645_ENC_PICTURE_TYPE_I:
      {
         return D3D12_VIDEO_ENCODER_FRAME_TYPE_H264_I_FRAME;
      } break;
      case PIPE_H2645_ENC_PICTURE_TYPE_IDR:
      {
         return D3D12_VIDEO_ENCODER_FRAME_TYPE_H264_IDR_FRAME;
      } break;
      default:
      {
         unreachable("Unsupported pipe_h2645_enc_picture_type");
      } break;
   }
}

///
/// Tries to configurate the encoder using the requested slice configuration
/// or falls back to single slice encoding.
///
bool
d3d12_video_encoder_negotiate_current_h264_slices_configuration(struct d3d12_video_encoder *pD3D12Enc,
                                                                pipe_h264_enc_picture_desc *picture)
{
   ///
   /// Initialize single slice by default
   ///
   D3D12_VIDEO_ENCODER_FRAME_SUBREGION_LAYOUT_MODE requestedSlicesMode =
      D3D12_VIDEO_ENCODER_FRAME_SUBREGION_LAYOUT_MODE_FULL_FRAME;
   D3D12_VIDEO_ENCODER_PICTURE_CONTROL_SUBREGIONS_LAYOUT_DATA_SLICES requestedSlicesConfig = {};
   requestedSlicesConfig.NumberOfSlicesPerFrame = 1;

   ///
   /// Try to see if can accomodate for multi-slice request by user
   ///
   if (picture->num_slice_descriptors > 1) {
      /* Last slice can be less for rounding frame size and leave some error for mb rounding */
      bool bUniformSizeSlices = true;
      const double rounding_delta = 1.0;
      for (uint32_t sliceIdx = 1; (sliceIdx < picture->num_slice_descriptors - 1) && bUniformSizeSlices; sliceIdx++) {
         int64_t curSlice = picture->slices_descriptors[sliceIdx].num_macroblocks;
         int64_t prevSlice = picture->slices_descriptors[sliceIdx - 1].num_macroblocks;
         bUniformSizeSlices = bUniformSizeSlices && (std::abs(curSlice - prevSlice) <= rounding_delta);
      }

      uint32_t mbPerScanline =
         pD3D12Enc->m_currentEncodeConfig.m_currentResolution.Width / D3D12_VIDEO_H264_MB_IN_PIXELS;
      bool bSliceAligned = ((picture->slices_descriptors[0].num_macroblocks % mbPerScanline) == 0);

      if (!bUniformSizeSlices &&
          d3d12_video_encoder_check_subregion_mode_support(
             pD3D12Enc,
             D3D12_VIDEO_ENCODER_FRAME_SUBREGION_LAYOUT_MODE_UNIFORM_PARTITIONING_SUBREGIONS_PER_FRAME)) {

         if (D3D12_VIDEO_ENC_FALLBACK_SLICE_CONFIG) {   // Check if fallback mode is enabled, or we should just fail
                                                        // without support
            // Not supported to have custom slice sizes in D3D12 Video Encode fallback to uniform multi-slice
            debug_printf(
               "[d3d12_video_encoder_h264] WARNING: Requested slice control mode is not supported: All slices must "
               "have the same number of macroblocks. Falling back to encoding uniform %d slices per frame.\n",
               picture->num_slice_descriptors);
            requestedSlicesMode =
               D3D12_VIDEO_ENCODER_FRAME_SUBREGION_LAYOUT_MODE_UNIFORM_PARTITIONING_SUBREGIONS_PER_FRAME;
            requestedSlicesConfig.NumberOfSlicesPerFrame = picture->num_slice_descriptors;
            debug_printf("[d3d12_video_encoder_h264] Using multi slice encoding mode: "
                           "D3D12_VIDEO_ENCODER_FRAME_SUBREGION_LAYOUT_MODE_UNIFORM_PARTITIONING_SUBREGIONS_PER_FRAME "
                           "with %d slices per frame.\n",
                           requestedSlicesConfig.NumberOfSlicesPerFrame);
         } else {
            debug_printf("[d3d12_video_encoder_h264] Requested slice control mode is not supported: All slices must "
                            "have the same number of macroblocks. To continue with uniform slices as a fallback, must "
                            "enable the OS environment variable D3D12_VIDEO_ENC_FALLBACK_SLICE_CONFIG");
            return false;
         }
      } else if (bUniformSizeSlices && bSliceAligned &&
                 d3d12_video_encoder_check_subregion_mode_support(
                    pD3D12Enc,
                    D3D12_VIDEO_ENCODER_FRAME_SUBREGION_LAYOUT_MODE_UNIFORM_PARTITIONING_ROWS_PER_SUBREGION)) {

         // Number of macroblocks per slice is aligned to a scanline width, in which case we can
         // use D3D12_VIDEO_ENCODER_FRAME_SUBREGION_LAYOUT_MODE_UNIFORM_PARTITIONING_ROWS_PER_SUBREGION
         requestedSlicesMode = D3D12_VIDEO_ENCODER_FRAME_SUBREGION_LAYOUT_MODE_UNIFORM_PARTITIONING_ROWS_PER_SUBREGION;
         requestedSlicesConfig.NumberOfRowsPerSlice = (picture->slices_descriptors[0].num_macroblocks / mbPerScanline);
         debug_printf("[d3d12_video_encoder_h264] Using multi slice encoding mode: "
                        "D3D12_VIDEO_ENCODER_FRAME_SUBREGION_LAYOUT_MODE_UNIFORM_PARTITIONING_ROWS_PER_SUBREGION with "
                        "%d macroblocks rows per slice.\n",
                        requestedSlicesConfig.NumberOfRowsPerSlice);
      } else if (bUniformSizeSlices &&
                 d3d12_video_encoder_check_subregion_mode_support(
                    pD3D12Enc,
                    D3D12_VIDEO_ENCODER_FRAME_SUBREGION_LAYOUT_MODE_UNIFORM_PARTITIONING_SUBREGIONS_PER_FRAME)) {
            requestedSlicesMode =
               D3D12_VIDEO_ENCODER_FRAME_SUBREGION_LAYOUT_MODE_UNIFORM_PARTITIONING_SUBREGIONS_PER_FRAME;
            requestedSlicesConfig.NumberOfSlicesPerFrame = picture->num_slice_descriptors;
            debug_printf("[d3d12_video_encoder_h264] Using multi slice encoding mode: "
                           "D3D12_VIDEO_ENCODER_FRAME_SUBREGION_LAYOUT_MODE_UNIFORM_PARTITIONING_SUBREGIONS_PER_FRAME "
                           "with %d slices per frame.\n",
                           requestedSlicesConfig.NumberOfSlicesPerFrame);
      } else if (D3D12_VIDEO_ENC_FALLBACK_SLICE_CONFIG) {   // Check if fallback mode is enabled, or we should just fail
                                                            // without support
         // Fallback to single slice encoding (assigned by default when initializing variables requestedSlicesMode,
         // requestedSlicesConfig)
         debug_printf(
            "[d3d12_video_encoder_h264] WARNING: Slice mode for %d slices with bUniformSizeSlices: %d - bSliceAligned "
            "%d not supported by the D3D12 driver, falling back to encoding a single slice per frame.\n",
            picture->num_slice_descriptors,
            bUniformSizeSlices,
            bSliceAligned);
      } else {
         debug_printf("[d3d12_video_encoder_h264] Requested slice control mode is not supported: All slices must "
                         "have the same number of macroblocks. To continue with uniform slices as a fallback, must "
                         "enable the OS environment variable D3D12_VIDEO_ENC_FALLBACK_SLICE_CONFIG");
         return false;
      }
   }

   if (!d3d12_video_encoder_compare_slice_config_h264_hevc(
          pD3D12Enc->m_currentEncodeConfig.m_encoderSliceConfigMode,
          pD3D12Enc->m_currentEncodeConfig.m_encoderSliceConfigDesc.m_SlicesPartition_H264,
          requestedSlicesMode,
          requestedSlicesConfig)) {
      pD3D12Enc->m_currentEncodeConfig.m_ConfigDirtyFlags |= d3d12_video_encoder_config_dirty_flag_slices;
   }

   pD3D12Enc->m_currentEncodeConfig.m_encoderSliceConfigDesc.m_SlicesPartition_H264 = requestedSlicesConfig;
   pD3D12Enc->m_currentEncodeConfig.m_encoderSliceConfigMode = requestedSlicesMode;

   return true;
}

D3D12_VIDEO_ENCODER_MOTION_ESTIMATION_PRECISION_MODE
d3d12_video_encoder_convert_h264_motion_configuration(struct d3d12_video_encoder *pD3D12Enc,
                                                      pipe_h264_enc_picture_desc *picture)
{
   return D3D12_VIDEO_ENCODER_MOTION_ESTIMATION_PRECISION_MODE_MAXIMUM;
}

D3D12_VIDEO_ENCODER_LEVELS_H264
d3d12_video_encoder_convert_level_h264(uint32_t h264SpecLevel)
{
   switch (h264SpecLevel) {
      case 10:
      {
         return D3D12_VIDEO_ENCODER_LEVELS_H264_1;
      } break;
      case 11:
      {
         return D3D12_VIDEO_ENCODER_LEVELS_H264_11;
      } break;
      case 12:
      {
         return D3D12_VIDEO_ENCODER_LEVELS_H264_12;
      } break;
      case 13:
      {
         return D3D12_VIDEO_ENCODER_LEVELS_H264_13;
      } break;
      case 20:
      {
         return D3D12_VIDEO_ENCODER_LEVELS_H264_2;
      } break;
      case 21:
      {
         return D3D12_VIDEO_ENCODER_LEVELS_H264_21;
      } break;
      case 22:
      {
         return D3D12_VIDEO_ENCODER_LEVELS_H264_22;
      } break;
      case 30:
      {
         return D3D12_VIDEO_ENCODER_LEVELS_H264_3;
      } break;
      case 31:
      {
         return D3D12_VIDEO_ENCODER_LEVELS_H264_31;
      } break;
      case 32:
      {
         return D3D12_VIDEO_ENCODER_LEVELS_H264_32;
      } break;
      case 40:
      {
         return D3D12_VIDEO_ENCODER_LEVELS_H264_4;
      } break;
      case 41:
      {
         return D3D12_VIDEO_ENCODER_LEVELS_H264_41;
      } break;
      case 42:
      {
         return D3D12_VIDEO_ENCODER_LEVELS_H264_42;
      } break;
      case 50:
      {
         return D3D12_VIDEO_ENCODER_LEVELS_H264_5;
      } break;
      case 51:
      {
         return D3D12_VIDEO_ENCODER_LEVELS_H264_51;
      } break;
      case 52:
      {
         return D3D12_VIDEO_ENCODER_LEVELS_H264_52;
      } break;
      case 60:
      {
         return D3D12_VIDEO_ENCODER_LEVELS_H264_6;
      } break;
      case 61:
      {
         return D3D12_VIDEO_ENCODER_LEVELS_H264_61;
      } break;
      case 62:
      {
         return D3D12_VIDEO_ENCODER_LEVELS_H264_62;
      } break;
      default:
      {
         unreachable("Unsupported H264 level");
      } break;
   }
}

void
d3d12_video_encoder_convert_from_d3d12_level_h264(D3D12_VIDEO_ENCODER_LEVELS_H264 level12,
                                                  uint32_t &specLevel,
                                                  uint32_t &constraint_set3_flag)
{
   specLevel = 0;
   constraint_set3_flag = 0;

   switch (level12) {
      case D3D12_VIDEO_ENCODER_LEVELS_H264_1:
      {
         specLevel = 10;
      } break;
      case D3D12_VIDEO_ENCODER_LEVELS_H264_1b:
      {
         specLevel = 11;
         constraint_set3_flag = 1;
      } break;
      case D3D12_VIDEO_ENCODER_LEVELS_H264_11:
      {
         specLevel = 11;
      } break;
      case D3D12_VIDEO_ENCODER_LEVELS_H264_12:
      {
         specLevel = 12;
      } break;
      case D3D12_VIDEO_ENCODER_LEVELS_H264_13:
      {
         specLevel = 13;
      } break;
      case D3D12_VIDEO_ENCODER_LEVELS_H264_2:
      {
         specLevel = 20;
      } break;
      case D3D12_VIDEO_ENCODER_LEVELS_H264_21:
      {
         specLevel = 21;
      } break;
      case D3D12_VIDEO_ENCODER_LEVELS_H264_22:
      {
         specLevel = 22;
      } break;
      case D3D12_VIDEO_ENCODER_LEVELS_H264_3:
      {
         specLevel = 30;
      } break;
      case D3D12_VIDEO_ENCODER_LEVELS_H264_31:
      {
         specLevel = 31;
      } break;
      case D3D12_VIDEO_ENCODER_LEVELS_H264_32:
      {
         specLevel = 32;
      } break;
      case D3D12_VIDEO_ENCODER_LEVELS_H264_4:
      {
         specLevel = 40;
      } break;
      case D3D12_VIDEO_ENCODER_LEVELS_H264_41:
      {
         specLevel = 41;
      } break;
      case D3D12_VIDEO_ENCODER_LEVELS_H264_42:
      {
         specLevel = 42;
      } break;
      case D3D12_VIDEO_ENCODER_LEVELS_H264_5:
      {
         specLevel = 50;
      } break;
      case D3D12_VIDEO_ENCODER_LEVELS_H264_51:
      {
         specLevel = 51;
      } break;
      case D3D12_VIDEO_ENCODER_LEVELS_H264_52:
      {
         specLevel = 52;
      } break;
      case D3D12_VIDEO_ENCODER_LEVELS_H264_6:
      {
         specLevel = 60;
      } break;
      case D3D12_VIDEO_ENCODER_LEVELS_H264_61:
      {
         specLevel = 61;
      } break;
      case D3D12_VIDEO_ENCODER_LEVELS_H264_62:
      {
         specLevel = 62;
      } break;
      default:
      {
         unreachable("Unsupported D3D12_VIDEO_ENCODER_LEVELS_H264 value");
      } break;
   }
}

bool
d3d12_video_encoder_update_h264_gop_configuration(struct d3d12_video_encoder *pD3D12Enc,
                                                  pipe_h264_enc_picture_desc *picture)
{
   // Only update GOP when it begins
   if (picture->gop_cnt == 1) {
      uint32_t GOPCoeff = picture->i_remain;
      uint32_t GOPLength = picture->gop_size / GOPCoeff;
      uint32_t PPicturePeriod = std::ceil(GOPLength / (double) (picture->p_remain / GOPCoeff)) - 1;

      if (picture->pic_order_cnt_type == 1u) {
         debug_printf("[d3d12_video_encoder_h264] Upper layer is requesting pic_order_cnt_type %d but D3D12 Video "
                         "only supports pic_order_cnt_type = 0 or pic_order_cnt_type = 2\n",
                         picture->pic_order_cnt_type);
         return false;
      }

      const uint32_t max_pic_order_cnt_lsb = 2 * GOPLength;
      const uint32_t max_max_frame_num = GOPLength;
      double log2_max_frame_num_minus4 = std::max(0.0, std::ceil(std::log2(max_max_frame_num)) - 4);
      double log2_max_pic_order_cnt_lsb_minus4 = std::max(0.0, std::ceil(std::log2(max_pic_order_cnt_lsb)) - 4);
      assert(log2_max_frame_num_minus4 < UCHAR_MAX);
      assert(log2_max_pic_order_cnt_lsb_minus4 < UCHAR_MAX);
      assert(picture->pic_order_cnt_type < UCHAR_MAX);

      // Set dirty flag if m_H264GroupOfPictures changed
      auto previousGOPConfig = pD3D12Enc->m_currentEncodeConfig.m_encoderGOPConfigDesc.m_H264GroupOfPictures;
      pD3D12Enc->m_currentEncodeConfig.m_encoderGOPConfigDesc.m_H264GroupOfPictures = {
         GOPLength,
         PPicturePeriod,
         static_cast<uint8_t>(picture->pic_order_cnt_type),
         static_cast<uint8_t>(log2_max_frame_num_minus4),
         static_cast<uint8_t>(log2_max_pic_order_cnt_lsb_minus4)
      };

      if (memcmp(&previousGOPConfig,
                 &pD3D12Enc->m_currentEncodeConfig.m_encoderGOPConfigDesc.m_H264GroupOfPictures,
                 sizeof(D3D12_VIDEO_ENCODER_SEQUENCE_GOP_STRUCTURE_H264)) != 0) {
         pD3D12Enc->m_currentEncodeConfig.m_ConfigDirtyFlags |= d3d12_video_encoder_config_dirty_flag_gop;
      }
   }
   return true;
}

D3D12_VIDEO_ENCODER_CODEC_CONFIGURATION_H264
d3d12_video_encoder_convert_h264_codec_configuration(struct d3d12_video_encoder *pD3D12Enc,
                                                     pipe_h264_enc_picture_desc *picture)
{
   D3D12_VIDEO_ENCODER_CODEC_CONFIGURATION_H264 config = {
      D3D12_VIDEO_ENCODER_CODEC_CONFIGURATION_H264_FLAG_NONE,
      D3D12_VIDEO_ENCODER_CODEC_CONFIGURATION_H264_DIRECT_MODES_DISABLED,
      D3D12_VIDEO_ENCODER_CODEC_CONFIGURATION_H264_SLICES_DEBLOCKING_MODE_0_ALL_LUMA_CHROMA_SLICE_BLOCK_EDGES_ALWAYS_FILTERED,
   };

   if (picture->pic_ctrl.enc_cabac_enable) {
      config.ConfigurationFlags |= D3D12_VIDEO_ENCODER_CODEC_CONFIGURATION_H264_FLAG_ENABLE_CABAC_ENCODING;
   }

   return config;
}

bool
d3d12_video_encoder_update_current_encoder_config_state_h264(struct d3d12_video_encoder *pD3D12Enc,
                                                             struct pipe_video_buffer *srcTexture,
                                                             struct pipe_picture_desc *picture)
{
   struct pipe_h264_enc_picture_desc *h264Pic = (struct pipe_h264_enc_picture_desc *) picture;

   // Reset reconfig dirty flags
   pD3D12Enc->m_currentEncodeConfig.m_ConfigDirtyFlags = d3d12_video_encoder_config_dirty_flag_none;
   // Reset sequence changes flags
   pD3D12Enc->m_currentEncodeConfig.m_seqFlags = D3D12_VIDEO_ENCODER_SEQUENCE_CONTROL_FLAG_NONE;

   // Set codec
   if (pD3D12Enc->m_currentEncodeConfig.m_encoderCodecDesc != D3D12_VIDEO_ENCODER_CODEC_H264) {
      pD3D12Enc->m_currentEncodeConfig.m_ConfigDirtyFlags |= d3d12_video_encoder_config_dirty_flag_codec;
   }
   pD3D12Enc->m_currentEncodeConfig.m_encoderCodecDesc = D3D12_VIDEO_ENCODER_CODEC_H264;

   // Set input format
   DXGI_FORMAT targetFmt = d3d12_convert_pipe_video_profile_to_dxgi_format(pD3D12Enc->base.profile);
   if (pD3D12Enc->m_currentEncodeConfig.m_encodeFormatInfo.Format != targetFmt) {
      pD3D12Enc->m_currentEncodeConfig.m_ConfigDirtyFlags |= d3d12_video_encoder_config_dirty_flag_input_format;
   }

   pD3D12Enc->m_currentEncodeConfig.m_encodeFormatInfo = {};
   pD3D12Enc->m_currentEncodeConfig.m_encodeFormatInfo.Format = targetFmt;
   HRESULT hr = pD3D12Enc->m_pD3D12Screen->dev->CheckFeatureSupport(D3D12_FEATURE_FORMAT_INFO,
                                                          &pD3D12Enc->m_currentEncodeConfig.m_encodeFormatInfo,
                                                          sizeof(pD3D12Enc->m_currentEncodeConfig.m_encodeFormatInfo));
   if (FAILED(hr)) {
      debug_printf("CheckFeatureSupport failed with HR %x\n", hr);
      return false;
   }

   // Set resolution
   if ((pD3D12Enc->m_currentEncodeConfig.m_currentResolution.Width != srcTexture->width) ||
       (pD3D12Enc->m_currentEncodeConfig.m_currentResolution.Height != srcTexture->height)) {
      pD3D12Enc->m_currentEncodeConfig.m_ConfigDirtyFlags |= d3d12_video_encoder_config_dirty_flag_resolution;
   }
   pD3D12Enc->m_currentEncodeConfig.m_currentResolution.Width = srcTexture->width;
   pD3D12Enc->m_currentEncodeConfig.m_currentResolution.Height = srcTexture->height;

   // Set resolution codec dimensions (ie. cropping)
   if (h264Pic->pic_ctrl.enc_frame_cropping_flag) {
      pD3D12Enc->m_currentEncodeConfig.m_FrameCroppingCodecConfig.left = h264Pic->pic_ctrl.enc_frame_crop_left_offset;
      pD3D12Enc->m_currentEncodeConfig.m_FrameCroppingCodecConfig.right = h264Pic->pic_ctrl.enc_frame_crop_right_offset;
      pD3D12Enc->m_currentEncodeConfig.m_FrameCroppingCodecConfig.top = h264Pic->pic_ctrl.enc_frame_crop_top_offset;
      pD3D12Enc->m_currentEncodeConfig.m_FrameCroppingCodecConfig.bottom =
         h264Pic->pic_ctrl.enc_frame_crop_bottom_offset;
   } else {
      memset(&pD3D12Enc->m_currentEncodeConfig.m_FrameCroppingCodecConfig,
             0,
             sizeof(pD3D12Enc->m_currentEncodeConfig.m_FrameCroppingCodecConfig));
   }

   // Set profile
   auto targetProfile = d3d12_video_encoder_convert_profile_to_d3d12_enc_profile_h264(pD3D12Enc->base.profile);
   if (pD3D12Enc->m_currentEncodeConfig.m_encoderProfileDesc.m_H264Profile != targetProfile) {
      pD3D12Enc->m_currentEncodeConfig.m_ConfigDirtyFlags |= d3d12_video_encoder_config_dirty_flag_profile;
   }
   pD3D12Enc->m_currentEncodeConfig.m_encoderProfileDesc.m_H264Profile = targetProfile;

   // Set level
   auto targetLevel = d3d12_video_encoder_convert_level_h264(pD3D12Enc->base.level);
   if (pD3D12Enc->m_currentEncodeConfig.m_encoderLevelDesc.m_H264LevelSetting != targetLevel) {
      pD3D12Enc->m_currentEncodeConfig.m_ConfigDirtyFlags |= d3d12_video_encoder_config_dirty_flag_level;
   }
   pD3D12Enc->m_currentEncodeConfig.m_encoderLevelDesc.m_H264LevelSetting = targetLevel;

   // Set codec config
   auto targetCodecConfig = d3d12_video_encoder_convert_h264_codec_configuration(pD3D12Enc, h264Pic);
   if (memcmp(&pD3D12Enc->m_currentEncodeConfig.m_encoderCodecSpecificConfigDesc.m_H264Config,
              &targetCodecConfig,
              sizeof(D3D12_VIDEO_ENCODER_CODEC_CONFIGURATION_H264)) != 0) {
      pD3D12Enc->m_currentEncodeConfig.m_ConfigDirtyFlags |= d3d12_video_encoder_config_dirty_flag_codec_config;
   }
   pD3D12Enc->m_currentEncodeConfig.m_encoderCodecSpecificConfigDesc.m_H264Config = targetCodecConfig;

   // Set rate control
   d3d12_video_encoder_update_current_rate_control_h264(pD3D12Enc, h264Pic);

   // Set slices config
   if(!d3d12_video_encoder_negotiate_current_h264_slices_configuration(pD3D12Enc, h264Pic)) {
      debug_printf("d3d12_video_encoder_negotiate_current_h264_slices_configuration failed!\n");
      return false;
   }

   // Set GOP config
   if(!d3d12_video_encoder_update_h264_gop_configuration(pD3D12Enc, h264Pic)) {
      debug_printf("d3d12_video_encoder_update_h264_gop_configuration failed!\n");
      return false;
   }

   // m_currentEncodeConfig.m_encoderPicParamsDesc pic params are set in d3d12_video_encoder_reconfigure_encoder_objects
   // after re-allocating objects if needed

   // Set motion estimation config
   auto targetMotionLimit = d3d12_video_encoder_convert_h264_motion_configuration(pD3D12Enc, h264Pic);
   if (pD3D12Enc->m_currentEncodeConfig.m_encoderMotionPrecisionLimit != targetMotionLimit) {
      pD3D12Enc->m_currentEncodeConfig.m_ConfigDirtyFlags |=
         d3d12_video_encoder_config_dirty_flag_motion_precision_limit;
   }
   pD3D12Enc->m_currentEncodeConfig.m_encoderMotionPrecisionLimit = targetMotionLimit;

   ///
   /// Check for video encode support detailed capabilities
   ///

   // Will call for d3d12 driver support based on the initial requested features, then
   // try to fallback if any of them is not supported and return the negotiated d3d12 settings
   D3D12_FEATURE_DATA_VIDEO_ENCODER_SUPPORT capEncoderSupportData = {};
   if (!d3d12_video_encoder_negotiate_requested_features_and_d3d12_driver_caps(pD3D12Enc, capEncoderSupportData)) {
      debug_printf("[d3d12_video_encoder_h264] After negotiating caps, D3D12_FEATURE_VIDEO_ENCODER_SUPPORT "
                      "arguments are not supported - "
                      "ValidationFlags: 0x%x - SupportFlags: 0x%x\n",
                      capEncoderSupportData.ValidationFlags,
                      capEncoderSupportData.SupportFlags);
      return false;
   }

   ///
   // Calculate current settings based on the returned values from the caps query
   //
   pD3D12Enc->m_currentEncodeCapabilities.m_MaxSlicesInOutput =
      d3d12_video_encoder_calculate_max_slices_count_in_output(
         pD3D12Enc->m_currentEncodeConfig.m_encoderSliceConfigMode,
         &pD3D12Enc->m_currentEncodeConfig.m_encoderSliceConfigDesc.m_SlicesPartition_H264,
         pD3D12Enc->m_currentEncodeCapabilities.m_currentResolutionSupportCaps.MaxSubregionsNumber,
         pD3D12Enc->m_currentEncodeConfig.m_currentResolution,
         pD3D12Enc->m_currentEncodeCapabilities.m_currentResolutionSupportCaps.SubregionBlockPixelsSize);

   //
   // Validate caps support returned values against current settings
   //
   if (pD3D12Enc->m_currentEncodeConfig.m_encoderProfileDesc.m_H264Profile !=
       pD3D12Enc->m_currentEncodeCapabilities.m_encoderSuggestedProfileDesc.m_H264Profile) {
      debug_printf("[d3d12_video_encoder_h264] Warning: Requested D3D12_VIDEO_ENCODER_PROFILE_H264 by upper layer: %d "
                    "mismatches UMD suggested D3D12_VIDEO_ENCODER_PROFILE_H264: %d\n",
                    pD3D12Enc->m_currentEncodeConfig.m_encoderProfileDesc.m_H264Profile,
                    pD3D12Enc->m_currentEncodeCapabilities.m_encoderSuggestedProfileDesc.m_H264Profile);
   }

   if (pD3D12Enc->m_currentEncodeConfig.m_encoderLevelDesc.m_H264LevelSetting !=
       pD3D12Enc->m_currentEncodeCapabilities.m_encoderLevelSuggestedDesc.m_H264LevelSetting) {
      debug_printf("[d3d12_video_encoder_h264] Warning: Requested D3D12_VIDEO_ENCODER_LEVELS_H264 by upper layer: %d "
                    "mismatches UMD suggested D3D12_VIDEO_ENCODER_LEVELS_H264: %d\n",
                    pD3D12Enc->m_currentEncodeConfig.m_encoderLevelDesc.m_H264LevelSetting,
                    pD3D12Enc->m_currentEncodeCapabilities.m_encoderLevelSuggestedDesc.m_H264LevelSetting);
   }

   if (pD3D12Enc->m_currentEncodeCapabilities.m_MaxSlicesInOutput >
       pD3D12Enc->m_currentEncodeCapabilities.m_currentResolutionSupportCaps.MaxSubregionsNumber) {
      debug_printf("[d3d12_video_encoder_h264] Desired number of subregions is not supported (higher than max "
                      "reported slice number in query caps)\n.");
      return false;
   }
   return true;
}

D3D12_VIDEO_ENCODER_PROFILE_H264
d3d12_video_encoder_convert_profile_to_d3d12_enc_profile_h264(enum pipe_video_profile profile)
{
   switch (profile) {
      case PIPE_VIDEO_PROFILE_MPEG4_AVC_CONSTRAINED_BASELINE:
      case PIPE_VIDEO_PROFILE_MPEG4_AVC_BASELINE:
      case PIPE_VIDEO_PROFILE_MPEG4_AVC_MAIN:
      {
         return D3D12_VIDEO_ENCODER_PROFILE_H264_MAIN;

      } break;
      case PIPE_VIDEO_PROFILE_MPEG4_AVC_HIGH:
      {
         return D3D12_VIDEO_ENCODER_PROFILE_H264_HIGH;
      } break;
      case PIPE_VIDEO_PROFILE_MPEG4_AVC_HIGH10:
      {
         return D3D12_VIDEO_ENCODER_PROFILE_H264_HIGH_10;
      } break;
      default:
      {
         unreachable("Unsupported pipe_video_profile");
      } break;
   }
}

D3D12_VIDEO_ENCODER_CODEC
d3d12_video_encoder_convert_codec_to_d3d12_enc_codec(enum pipe_video_profile profile)
{
   switch (u_reduce_video_profile(profile)) {
      case PIPE_VIDEO_FORMAT_MPEG4_AVC:
      {
         return D3D12_VIDEO_ENCODER_CODEC_H264;
      } break;
      case PIPE_VIDEO_FORMAT_HEVC:
      {
         return D3D12_VIDEO_ENCODER_CODEC_HEVC;
      } break;
      case PIPE_VIDEO_FORMAT_MPEG12:
      case PIPE_VIDEO_FORMAT_MPEG4:
      case PIPE_VIDEO_FORMAT_VC1:
      case PIPE_VIDEO_FORMAT_JPEG:
      case PIPE_VIDEO_FORMAT_VP9:
      case PIPE_VIDEO_FORMAT_UNKNOWN:
      default:
      {
         unreachable("Unsupported pipe_video_profile");
      } break;
   }
}

bool
d3d12_video_encoder_compare_slice_config_h264_hevc(
   D3D12_VIDEO_ENCODER_FRAME_SUBREGION_LAYOUT_MODE targetMode,
   D3D12_VIDEO_ENCODER_PICTURE_CONTROL_SUBREGIONS_LAYOUT_DATA_SLICES targetConfig,
   D3D12_VIDEO_ENCODER_FRAME_SUBREGION_LAYOUT_MODE otherMode,
   D3D12_VIDEO_ENCODER_PICTURE_CONTROL_SUBREGIONS_LAYOUT_DATA_SLICES otherConfig)
{
   return (targetMode == otherMode) &&
          (memcmp(&targetConfig,
                  &otherConfig,
                  sizeof(D3D12_VIDEO_ENCODER_PICTURE_CONTROL_SUBREGIONS_LAYOUT_DATA_SLICES)) == 0);
}

uint32_t
d3d12_video_encoder_build_codec_headers_h264(struct d3d12_video_encoder *pD3D12Enc)
{
   D3D12_VIDEO_ENCODER_PICTURE_CONTROL_CODEC_DATA currentPicParams =
      d3d12_video_encoder_get_current_picture_param_settings(pD3D12Enc);

   auto profDesc = d3d12_video_encoder_get_current_profile_desc(pD3D12Enc);
   auto levelDesc = d3d12_video_encoder_get_current_level_desc(pD3D12Enc);
   auto codecConfigDesc = d3d12_video_encoder_get_current_codec_config_desc(pD3D12Enc);
   auto MaxDPBCapacity = d3d12_video_encoder_get_current_max_dpb_capacity(pD3D12Enc);

   size_t writtenSPSBytesCount = 0;
   bool isFirstFrame = (pD3D12Enc->m_fenceValue == 1);
   bool writeNewSPS = isFirstFrame                                         // on first frame
                      || ((pD3D12Enc->m_currentEncodeConfig.m_seqFlags &   // also on resolution change
                           D3D12_VIDEO_ENCODER_SEQUENCE_CONTROL_FLAG_RESOLUTION_CHANGE) != 0);

   d3d12_video_bitstream_builder_h264 *pH264BitstreamBuilder =
      dynamic_cast<d3d12_video_bitstream_builder_h264 *>(pD3D12Enc->m_upBitstreamBuilder.get());
   assert(pH264BitstreamBuilder);

   uint32_t active_seq_parameter_set_id = pH264BitstreamBuilder->get_active_sps_id();

   if (writeNewSPS) {
      // For every new SPS for reconfiguration, increase the active_sps_id
      if (!isFirstFrame) {
         active_seq_parameter_set_id++;
         pH264BitstreamBuilder->set_active_sps_id(active_seq_parameter_set_id);
      }
      pH264BitstreamBuilder->build_sps(*profDesc.pH264Profile,
                                       *levelDesc.pH264LevelSetting,
                                       pD3D12Enc->m_currentEncodeConfig.m_encodeFormatInfo.Format,
                                       *codecConfigDesc.pH264Config,
                                       pD3D12Enc->m_currentEncodeConfig.m_encoderGOPConfigDesc.m_H264GroupOfPictures,
                                       active_seq_parameter_set_id,
                                       MaxDPBCapacity,   // max_num_ref_frames
                                       pD3D12Enc->m_currentEncodeConfig.m_currentResolution,
                                       pD3D12Enc->m_currentEncodeConfig.m_FrameCroppingCodecConfig,
                                       pD3D12Enc->m_BitstreamHeadersBuffer,
                                       pD3D12Enc->m_BitstreamHeadersBuffer.begin(),
                                       writtenSPSBytesCount);
   }

   size_t writtenPPSBytesCount = 0;
   pH264BitstreamBuilder->build_pps(*profDesc.pH264Profile,
                                    *codecConfigDesc.pH264Config,
                                    *currentPicParams.pH264PicData,
                                    currentPicParams.pH264PicData->pic_parameter_set_id,
                                    active_seq_parameter_set_id,
                                    pD3D12Enc->m_BitstreamHeadersBuffer,
                                    pD3D12Enc->m_BitstreamHeadersBuffer.begin() + writtenSPSBytesCount,
                                    writtenPPSBytesCount);

   // Shrink buffer to fit the headers
   if (pD3D12Enc->m_BitstreamHeadersBuffer.size() > (writtenPPSBytesCount + writtenSPSBytesCount)) {
      pD3D12Enc->m_BitstreamHeadersBuffer.resize(writtenPPSBytesCount + writtenSPSBytesCount);
   }

   return pD3D12Enc->m_BitstreamHeadersBuffer.size();
}
