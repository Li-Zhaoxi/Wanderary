#include <BPU/bpu.h>
#include <Core/core.h>

namespace wdr
{

  namespace BPU
  {
    ////////////////////////////// TensorSize //////////////////////////////
    bool TensorSize::operator==(const TensorSize &tz) const
    {
      int d = dims();
      int dsz = tz.dims();
      if (d != dsz)
        return false;

      for (int i = 0; i < d; i++)
        if (shapes[i] != tz.shapes[i])
          return false;
      return true;
    }

    bool TensorSize::operator<=(const TensorSize &tz) const
    {
      int d = dims();
      int dsz = tz.dims();
      if (d != dsz)
        return false;
      for (int i = 0; i < d; i++)
        if (shapes[i] > tz.shapes[i])
          return false;
      return true;
    }

    bool TensorSize::operator>=(const TensorSize &tz) const
    {
      int d = dims();
      int dsz = tz.dims();
      if (d != dsz)
        return false;
      for (int i = 0; i < d; i++)
        if (shapes[i] < tz.shapes[i])
          return false;
      return true;
    }

    ////////////////////////////// BpuMat //////////////////////////////
    void BpuMat::update()
    {
      if (idxtensor < 0)
        validdims.clear(), aligneddims.clear(), alignedByteSize = 0, tensorLayout = 0;
      else
      {
        const auto &property = properties->infos.at(idxtensor);
        validdims.resize(property.validShape.numDimensions);
        aligneddims.resize(property.alignedShape.numDimensions);
        CV_Assert(validdims.size() == aligneddims.size() && validdims.size() == 4);

        for (int k = 0; k < validdims.size(); k++)
        {
          validdims[k] = property.validShape.dimensionSize[k];
          aligneddims[k] = property.alignedShape.dimensionSize[k];
        }
        alignedByteSize = property.alignedByteSize;
        tensorLayout = property.tensorLayout;
      }
    }

    bool BpuMat::empty() const
    {
      if (alignedByteSize <= 0)
        return false;
      else
        return true;
    }

    int BpuMat::batchsize(bool aligned) const
    {
      if (empty())
        return 0;
      return aligned ? aligneddims[0] : validdims[0];
    }

    int BpuMat::channels(bool aligned) const
    {
      if (empty())
        return 0;
      int idx = tensorLayout == HB_DNN_LAYOUT_NHWC ? 3 : 1;
      return aligned ? aligneddims[idx] : validdims[idx];
    }

    cv::Size BpuMat::size(bool aligned) const
    {
      if (empty())
        return cv::Size(0, 0);

      int idxh = tensorLayout == HB_DNN_LAYOUT_NHWC ? 1 : 2;
      return aligned ? cv::Size(aligneddims[idxh + 1], aligneddims[idxh]) : cv::Size(validdims[idxh + 1], validdims[idxh]);
    }

    int BpuMat::total(bool aligned) const
    {
      if (empty())
        return 0;

      int res = 1;
      for (int k = 0; k < aligneddims.size(); k++)
        res *= (aligned ? aligneddims[k] : validdims[k]);

      return res;
    }

    size_t BpuMat::elemSize() const
    {
      if (empty())
        return 0;

      return alignedByteSize / total(true);
    }

    void BpuMat::shape(std::vector<int> dims, bool aligned = false) const
    {
      dims.clear();
      if (empty())
        return;

      const auto &property = properties->infos[idxtensor];
      if (aligned)
      {
        const int dimnum = property.alignedShape.numDimensions;
        dims.resize(dimnum);
        for (int k = 0; k < dimnum; k++)
          dims[k] = property.alignedShape.dimensionSize[k];
      }
      else
      {
        const int dimnum = property.validShape.numDimensions;
        dims.resize(dimnum);
        for (int k = 0; k < dimnum; k++)
          dims[k] = property.validShape.dimensionSize[k];
      }
    }

    void BpuMat::copyFrom(cv::InputArray cvmat)
    {
      CV_Assert(!empty());
      const auto &property = properties->infos[idxtensor];

      //// 拷贝数据约束内存一定是对齐的
      cv::Mat mat, tmp;
      if (cvmat.rows() > 0 && property.tensorLayout != HB_DNN_LAYOUT_NHWC)
        hwc_to_chw(cvmat, tmp);
      else
        tmp = cvmat.getMat();

      if (tmp.isContinuous())
        mat = tmp;
      else
        tmp.copyTo(mat);

      // 统计输入数据的维度
      std::vector<int> dims;
      if (mat.rows < 0 || mat.cols < 0)
      {
        int srcdims = mat.size.dims();
        CV_Assert(srcdims == 4 || srcdims == 3);
        if (srcdims == 3)
          dims.push_back(1);
        for (int k = 0; k < srcdims; k++)
          dims.push_back(mat.size[k]);
      }
      else // 走到这里一定是排布满足NHWC了
      {
        int nc = mat.channels(), nh = mat.rows, nw = mat.cols;
        dims.resize(4);
        dims[0] = 1, dims[1] = nh, dims[2] = nw, dims[3] = nc;
      }

      // 检查当前维度与validShape还是alignedShape匹配
      bool matchvalid = false, matchaliged = false;
      std::vector<int> dimvalid, dimalign;
      shape(dimvalid, false), shape(dimalign, true);
      CV_Assert(dims.size() == dimvalid.size() && dims.size() == dimalign.size());
    }

    ////////////////////////////// BpuMats //////////////////////////////
    BpuMats::BpuMats()
    {
      range = cv::Range(0, 0);
      matset = nullptr;
      properties = nullptr;
      dev = nullptr;
    }

    BpuMats::~BpuMats()
    {
      this->release();
    }

    void BpuMats::release()
    {
      range = cv::Range(0, 0);
      matset.reset(), properties.reset(), dev.reset();
      matset = nullptr, properties = nullptr, dev = nullptr;
    }

    void BpuMats::create(const NetIOInfo &infos)
    {
      this->release();
      properties = std::make_shared<NetIOInfo>(infos);
      const int num = properties->size();
      matset = std::make_shared<std::vector<hbDNNTensor>>();
      createTensors(properties->infos, *matset, false);

      dev = std::make_shared<DEVICE>();
      *dev = DEVICE::NET_CPU;
    }

    BpuMats BpuMats::operator()(cv::Range &_range) const
    {
      CV_Assert(_range.start <= _range.end && _range.start >= 0);
      CV_Assert((_range.end - _range.start) <= this->range.size());

      BpuMats res = *this;
      res.range.start = range.start + _range.start;
      res.range.end = range.start + _range.end;

      return res;
    }

    BpuMat BpuMats::operator[](int idx) const
    {
      CV_Assert(idx >= 0 && range.start + idx < range.end);
      if (this->device() != DEVICE::NET_CPU)
        CV_Error(cv::Error::StsError, "Output Tensors are not in CPU, Please call input_mats.cpu() before this function.");

      BpuMat res;
      res.properties = this->properties;
      res.matset = this->matset;
      res.idxtensor = range.start + idx;
      res.update();
      return res;
    }

    void BpuMats::bpu()
    {
      CV_Assert(dev != nullptr);
      if (*dev == DEVICE::NET_CPU)
      {
        for (int k = 0; k < matset->size(); k++)
        {
          auto &src = matset->at(k);
          HB_CHECK_SUCCESS(hbSysFlushMem(&src.sysMem[0], HB_SYS_MEM_CACHE_CLEAN),
                           "hbSysFlushMem cpu->tensor failed");
        }
        *dev == DEVICE::NET_BPU;
      }
    }

    void BpuMats::cpu()
    {
      CV_Assert(dev != nullptr);
      if (*dev == DEVICE::NET_BPU)
      {
        for (int k = 0; k < matset->size(); k++)
        {
          auto &src = matset->at(k);
          HB_CHECK_SUCCESS(hbSysFlushMem(&src.sysMem[0], HB_SYS_MEM_CACHE_INVALIDATE),
                           "hbSysFlushMem tensor->cpu failed");
        }
        *dev == DEVICE::NET_CPU;
      }
    }

  } // end BPU
} // end wdr