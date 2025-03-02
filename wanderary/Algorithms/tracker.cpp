#include <Algorithms/tracker.h>

namespace wdr
{
  float calTargetLength(const cv::Size2f &target, float thre_context)
  {
    // context = 1/2 * (w+h) = 2*pad
    const float lwh = (target.height + target.width) * thre_context;
    float wc_z = target.width + lwh, hc_z = target.height + lwh;
    return std::sqrt(wc_z * hc_z);
  }

  float calTargetLength(float tw, float th, float thre_context)
  {
    return calTargetLength(cv::Size2f(tw, th), thre_context);
  }

  void get_subwindow_tracking(const cv::Mat &src, cv::Mat &dst, const cv::Rect2f &croproi, bool usecenter, const cv::Size &modelsize)
  {
    // 构建ROI的xy的最大值和最小值
    int context_xmin{0}, context_xmax{0}, context_ymin{0}, context_ymax{0};
    if (usecenter)
    {
      float cw = (croproi.width + 1.0f) / 2.0f, ch = (croproi.height + 1.0f) / 2.0f;
      context_xmin = int(croproi.x - cw + 0.5), context_xmax = int(context_xmin + croproi.width + 0.5);
      context_ymin = int(croproi.y - ch + 0.5), context_ymax = int(context_ymin + croproi.height + 0.5);
    }
    else
    {
      context_xmin = int(croproi.x), context_xmax = int(context_xmin + croproi.width + 0.5);
      context_ymin = int(croproi.y), context_ymax = int(context_ymin + croproi.height + 0.5);
    }

    // 计算ROI的越界情况
    int left_pad{0}, top_pad{0}, right_pad{0}, bottom_pad{0};
    left_pad = std::max(0, -context_xmin), top_pad = std::max(0, -context_ymin);
    right_pad = std::max(0, context_xmax - src.cols), bottom_pad = std::max(0, context_ymax - src.rows);

    cv::Mat imgroi;
    if (left_pad == 0 && top_pad == 0 && right_pad == 0 && bottom_pad == 0)
    {
      cv::Rect roi(context_xmin, context_ymin, context_xmax - context_xmin, context_ymax - context_ymin);
      imgroi = src(roi);
      // LOG(INFO) << "roi: " << roi;
    }
    else
    {
      cv::Rect srcroi(context_xmin + left_pad, context_ymin + top_pad, context_xmax - right_pad, context_ymax - bottom_pad);
      srcroi.width = srcroi.width - srcroi.x, srcroi.height = srcroi.height - srcroi.y;
      cv::Rect dstroi(left_pad, top_pad, srcroi.width, srcroi.height);
      // LOG(INFO) << "srcroi: " << srcroi;
      // LOG(INFO) << "dstroi: " << dstroi;

      // 初始化
      imgroi.create(top_pad + dstroi.height, left_pad + dstroi.width, CV_MAKETYPE(src.depth(), src.channels()));
      imgroi.setTo(cv::mean(src));
      // LOG(INFO) << "src size: " << src.size() << ", imgroi size: " << imgroi.size();
      src(srcroi).copyTo(imgroi(dstroi));
    }

    cv::resize(imgroi, dst, modelsize);
  }

  // lsrc估计出的边长，ldst目标边长，target实际的目标框
  cv::Rect2f estRectInCrop(float lsrc, float ldst, const cv::Size2f &target)
  {
    float scale = ldst / lsrc;
    float w = target.width * scale, h = target.height * scale;

    cv::Rect2f res;
    // cx, cy = (ldst - 1)
    res.x = (ldst - 1) / 2.0f - w / 2.0f;
    res.y = (ldst - 1) / 2.0f - h / 2.0f;
    res.width = w, res.height = h;
    return res;
  }

  cv::Rect2f recoverNewBox(const cv::Size2f &crop, const cv::Rect2f &pred,
                           const cv::Rect2f &src,
                           const cv::Point2f &scalexy, float lr)
  {
    cv::Rect2f newbox;

    ////// 估计实际中心点
    // 计算估计的目标框相对于输入目标框的偏差
    float pred_xs = pred.x + pred.width / 2, pred_ys = pred.y + pred.height / 2;
    float diff_xs = pred_xs - int(crop.width / 2);
    float diff_ys = pred_ys - int(crop.height / 2);
    diff_xs /= scalexy.x, diff_ys /= scalexy.y;

    float src_cx = src.x + src.width / 2;
    float src_cy = src.y + src.height / 2;
    newbox.x = src_cx + diff_xs, newbox.y = src_cy + diff_ys;

    ////// 估计实际尺寸（加权）
    newbox.width = src.width * (1 - lr) + pred.width * lr;
    newbox.height = src.height * (1 - lr) + pred.height * lr;
    newbox.width /= scalexy.x, newbox.height /= scalexy.y;

    newbox.x -= newbox.width / 2, newbox.y -= newbox.height / 2;

    return newbox;
  }

  cv::Rect normBox(const cv::Rect2f &box, const cv::Size &size)
  {
    cv::Rect res;
    res.x = std::max(0, int(box.x + 0.5));
    res.y = std::max(0, int(box.y + 0.5));
    res.width = std::min(size.width, int(box.width + 0.5));
    res.height = std::min(size.height, int(box.height + 0.5));

    return res;
  }

  cv::Rect2f estimateTrackRect(const cv::Mat &anchorx, const cv::Mat &anchory,
                               const cv::Mat &window,
                               const cv::Rect2f &sz_in,
                               const cv::Mat &scores, const cv::Mat &preds,
                               const cv::Point2f &scalexy,
                               const cv::Size2f &instance_size,
                               float penalty_k,
                               float window_influence,
                               float lr)
  {
    // Lambda 函数集
    auto fun_sz = [](float w, float h)
    {
      float pad = (w + h) * 0.5f;
      return std::sqrt((w + pad) * (h + pad));
    };

    auto fun_rnorm = [](float r)
    {
      return std::max(r, 1.0f / r);
    };

    // 确认维度，确认数据类型，确认内存连续
    const int rows = anchorx.rows, cols = anchorx.cols, total = rows * cols;
    // dim: 4 [1x1x31x31]" dim: 4 [1x4x31x31]",
    BPU::TensorSize srcscoresize(wdr::shape(scores), true), dstscoresize({rows, cols}, true);
    BPU::TensorSize srcbboxsize(wdr::shape(preds), true), dstbboxsize({4, rows, cols}, true);

    if (srcscoresize != dstscoresize)
    {
      std::stringstream ss;
      ss << "Invalid score shape, input score shape: " << srcscoresize << ", dst score shape: " << dstscoresize;
      CV_Error(cv::Error::StsAssert, ss.str());
    }
    if (srcbboxsize != dstbboxsize)
    {
      std::stringstream ss;
      ss << "Invalid pred bbox shape, input pred bbox shape: " << srcbboxsize << ", dst pred bbox shape: " << dstbboxsize;
      CV_Error(cv::Error::StsAssert, ss.str());
    }
    CV_Assert(scores.isContinuous() && preds.isContinuous());

    // 预分配所有估计出的框
    std::vector<cv::Rect2f> bboxs(total);
    cv::Mat costs(1, total, CV_64FC1), pscore(1, total, CV_32FC1); // CV_64FC1 方便后续计算最大值用
    double *_costs = (double *)costs.data;
    float *_pscore = (float *)pscore.data;

    // 多线程解算目标框
    float ltar_diag = fun_sz(sz_in.width, sz_in.height);
    float rtar_wh = sz_in.width / sz_in.height;

    float *_anchorx = (float *)anchorx.data, *_anchory = (float *)anchory.data;
    float *_window = (float *)window.data;
    float *_scores = (float *)scores.data, *_preds = (float *)preds.data;

    auto fun_cal_bbox = [&](cv::Range range)
    {
      for (int r = range.start; r < range.end; r++)
      {
        int idxr = r / cols, idxc = r % cols;

        // 1. 估计左上角和右下角预测点
        cv::Point2f lt, br;
        {
          float anchorx = _anchorx[r], anchory = _anchory[r];
          lt.x = anchorx - _preds[r];
          lt.y = anchory - _preds[r + total];
          br.x = anchorx + _preds[r + total * 2];
          br.y = anchory + _preds[r + total * 3];
        }
        cv::Size2f wh(br.x - lt.x, br.y - lt.y);

        // 2. 估计预测评分
        // 2.1 尺寸/宽高比关联的损失值
        float cost_sr;
        {
          float score = wdr::sigmode(_scores[r]);
          // size penalty: 两个框带0.5pad的对角线长度比值
          float cost_size = fun_rnorm(fun_sz(wh.width, wh.height) / ltar_diag);
          // ratio penalty: 两个框的宽高比的比值
          float cost_ratio = fun_rnorm((wh.width / wh.height / rtar_wh));
          cost_sr = std::exp(-(cost_size * cost_ratio - 1) * penalty_k) * score;
        }
        // 2.2 引入window损失，得到最终评分
        float score_final = cost_sr * (1 - window_influence) + _window[r] * window_influence;
        // 结果赋值
        auto &rect = bboxs[r];
        rect.x = lt.x, rect.y = lt.y;
        rect.width = wh.width, rect.height = wh.height;
        _pscore[r] = cost_sr;
        _costs[r] = score_final;
        // LOG(INFO) << "rect: " << rect;
        // LOG(INFO) << "_pscore: " << _pscore[r];
        // LOG(INFO) << "_costs: " << _costs[r];
        // LOG(INFO) << "r: " << r << ", cost: " << _costs[r] << ", rect: " << rect;
        // LOG(INFO) << "r: " << r << ", lt: " << lt << ", br: " << br << ", score: " << wdr::sigmode(_scores[r]) << ", bbox_pred0: " << _preds[r] << ", bbox_pred1: " << _preds[r + total] << ", bbox_pred2: " << _preds[r + total * 2] << ", bbox_pred3: " << _preds[r + total * 3];
      }
    };
    cv::parallel_for_(cv::Range(0, total), fun_cal_bbox);
    // fun_cal_bbox(cv::Range(0, total));

    // 计算最大值
    double max_score = 0;
    int idxs_max[2] = {-1, -1};
    cv::minMaxIdx(costs, nullptr, &max_score, nullptr, idxs_max);
    int idx_max = idxs_max[0] * costs.cols + idxs_max[1];

    // LOG(INFO) << "idxs_max: " << cv::Point(idxs_max[0], idxs_max[1]) << ", max_score: " << max_score;

    // 还原最终框，预测的实际上是偏差
    cv::Rect2f finalbox = bboxs[idx_max];
    float adapt_lr = _pscore[idx_max] * lr;

    // LOG(INFO) << "instance_size: " << instance_size << ", finalbox: " << finalbox << ", sz_in: " << sz_in << ", scalexy: " << scalexy << ", adapt_lr: " << adapt_lr;

    cv::Rect2f predbox = recoverNewBox(instance_size, finalbox, sz_in, scalexy, adapt_lr);
    // LOG(INFO) << "predbox: " << predbox;
    // CV_Assert(0);
    return predbox;
  }

  void grids(const cv::Size &size, cv::Mat &gridx, cv::Mat &gridy)
  {
    int rows = size.height, cols = size.width;

    gridx.create(rows, cols, CV_32FC1), gridy.create(rows, cols, CV_32FC1);
    float *_gridx = (float *)gridx.data, *_gridy = (float *)gridy.data;

    for (int i = 0; i < rows; i++)
    {
      for (int j = 0; j < cols; j++)
      {
        *_gridx = j, *_gridy = i;
        _gridx++, _gridy++;
      }
    }
  }

}