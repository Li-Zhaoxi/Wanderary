import cv2
import copy
import numpy as np
from lib.utils.deploy_helper import get_subwindow_tracking, python2round, bgr2nv12_opencv, get_bbox


class DCMT_deploy(object):
    def __init__(self, config):
        super(DCMT_deploy, self).__init__()
        self.config = config  # model and benchmark info
        self.stride = config.MODEL.STRIDE

    def init(self, im, target_pos, target_sz, model):
        state = dict()
        
        state['im_h'] = im.shape[0]
        state['im_w'] = im.shape[1]

        p = self.config.DEMO.HYPERS
        p.exemplar_size = self.config.TRAIN.TEMPLATE_SIZE
        p.instance_size = self.config.TRAIN.SEARCH_SIZE

        p.score_size = 31

        self.grids(p)  # self.grid_to_search_x, self.grid_to_search_y

        net = model

        wc_z = target_sz[0] + p.context_amount * sum(target_sz)
        hc_z = target_sz[1] + p.context_amount * sum(target_sz)
        s_z = round(np.sqrt(wc_z * hc_z))

        avg_chans = np.mean(im, axis=(0, 1))
        z_crop, _ = get_subwindow_tracking(im, target_pos, p.exemplar_size, s_z, avg_chans)
        z_bgr = np.expand_dims(z_crop, axis=0)
        # z_bgr = z_bgr.astype(np.uint8)

        z_box = get_bbox(s_z, p, target_sz)
        z_box = np.array(list(z_box), dtype=np.float32)
        z_box = np.expand_dims(z_box, axis=0)
        z_box = np.expand_dims(z_box, axis=-1)
        z_box = np.expand_dims(z_box, axis=-1)
        
        net.template(z_bgr, z_box)

        window = np.outer(np.hanning(p.score_size), np.hanning(p.score_size))  # [31, 31]
        # print(z_bgr.shape)
        # cv2.imwrite("z_bgr_py.png", z_bgr[0])
        # print(f"z_box: {z_box}")
        # exit()

        state['p'] = p
        state['net'] = net
        state['avg_chans'] = avg_chans
        state['window'] = window
        state['target_pos'] = target_pos
        state['target_sz'] = target_sz
        
        return state

    def track(self, state, im, savedebug = None):
        p = state['p']
        net = state['net']
        avg_chans = state['avg_chans']
        window = state['window']
        target_pos = state['target_pos']
        target_sz = state['target_sz']
        
        if savedebug is not None and isinstance(savedebug, dict):
            savedebug["target_pos_in"] = target_pos
            savedebug["target_sz_in"] = target_sz
        
        hc_z = target_sz[1] + p.context_amount * sum(target_sz)
        wc_z = target_sz[0] + p.context_amount * sum(target_sz)
        s_z = np.sqrt(wc_z * hc_z)
        scale_z = p.exemplar_size / s_z
        d_search = (p.instance_size - p.exemplar_size) / 2  # slightly different from rpn++
        pad = d_search / scale_z
        s_x = s_z + 2 * pad
        
        

        x_crop, crop_info = get_subwindow_tracking(im, target_pos, p.instance_size, python2round(s_x), avg_chans)
        state['x_crop'] = copy.deepcopy(x_crop)  # torch float tensor, (3,H,W)
        if savedebug is not None and isinstance(savedebug, dict):
            savedebug["crop_info"] = crop_info

        x_bgr = np.expand_dims(x_crop, axis=0)
        # x_bgr = x_bgr.astype(np.uint8)
        target_pos, target_sz, _, usage_time = self.update(net, x_bgr, target_pos, target_sz * scale_z, window, scale_z, p)
        target_pos[0] = max(0, min(state['im_w'], target_pos[0]))
        target_pos[1] = max(0, min(state['im_h'], target_pos[1]))
        target_sz[0] = max(10, min(state['im_w'], target_sz[0]))
        target_sz[1] = max(10, min(state['im_h'], target_sz[1]))
        
        # print(f"target_pos: {target_pos}, target_sz: {target_sz}")
        # exit()
        state['target_pos'] = target_pos
        state['target_sz'] = target_sz
        state['p'] = p
        state['infertime'] = usage_time

        return state

    def update(self, net, x_crops, target_pos, target_sz, window, scale_z, p, savedebug=None):
        # print("x_crops.shape", x_crops.shape)
        # cv2.imwrite("x_crops_py.png", x_crops[0])
        t1 = cv2.getTickCount()
        cls_score, bbox_pred = net.track(x_crops, savedebug)
        t2 = cv2.getTickCount()
        usage_time = (t2 - t1) * 1000 / cv2.getTickFrequency()

        cls_score = self.sigmoid(cls_score.squeeze())
        bbox_pred = bbox_pred.squeeze()

        pred_x1 = self.grid_to_search_x - bbox_pred[0, ...]
        pred_y1 = self.grid_to_search_y - bbox_pred[1, ...]
        pred_x2 = self.grid_to_search_x + bbox_pred[2, ...]
        pred_y2 = self.grid_to_search_y + bbox_pred[3, ...]

        # size penalty
        s_c = self.change(self.sz(pred_x2 - pred_x1, pred_y2 - pred_y1) / (self.sz_wh(target_sz)))  # scale penalty
        r_c = self.change((target_sz[0] / target_sz[1]) / ((pred_x2 - pred_x1) / (pred_y2 - pred_y1)))  # ratio penalty

        penalty = np.exp(-(r_c * s_c - 1) * p.penalty_k)
        pscore = penalty * cls_score
        # window penalty
        pscore = pscore * (1 - p.window_influence) + window * p.window_influence
        
        # get max
        r_max, c_max = np.unravel_index(pscore.argmax(), pscore.shape)
        print(f"idxs_max: [{r_max}, {c_max}], max_score: {pscore[r_max, c_max]}, cls_score: {cls_score[r_max, c_max]}")
        # to real size
        pred_x1 = pred_x1[r_max, c_max]
        pred_y1 = pred_y1[r_max, c_max]
        pred_x2 = pred_x2[r_max, c_max]
        pred_y2 = pred_y2[r_max, c_max]
        print(f"pred_x1: {pred_x1}, pred_y1: {pred_y1}, pred_x2: {pred_x2}, pred_y2: {pred_y2}")
        print(f"bbox_pred0: {bbox_pred[0, r_max, c_max]}, bbox_pred1: {bbox_pred[1, r_max, c_max]}, bbox_pred2: {bbox_pred[2, r_max, c_max]}, bbox_pred3: {bbox_pred[3, r_max, c_max]}")
        print(f"anchorx: {self.grid_to_search_x[r_max, c_max]}, anchory: {self.grid_to_search_y[r_max, c_max]}")
        print(f"instance_size: {p.instance_size}, sz_in: {target_sz},{target_pos}, scalexy: {scale_z}")
        pred_xs = (pred_x1 + pred_x2) / 2
        pred_ys = (pred_y1 + pred_y2) / 2
        pred_w = pred_x2 - pred_x1
        pred_h = pred_y2 - pred_y1

        diff_xs = pred_xs - p.instance_size // 2
        diff_ys = pred_ys - p.instance_size // 2

        diff_xs, diff_ys, pred_w, pred_h = diff_xs / scale_z, diff_ys / scale_z, pred_w / scale_z, pred_h / scale_z

        target_sz = target_sz / scale_z

        # size learning rate
        lr = penalty[r_max, c_max] * cls_score[r_max, c_max] * p.lr
        print(f"adapt_lr: {lr}")
        # size rate
        res_xs = target_pos[0] + diff_xs
        res_ys = target_pos[1] + diff_ys
        res_w = pred_w * lr + (1 - lr) * target_sz[0]
        res_h = pred_h * lr + (1 - lr) * target_sz[1]

        target_pos = np.array([res_xs, res_ys])
        # target_sz = target_sz * (1 - lr) + lr * np.array([res_w, res_h])
        target_sz = np.array([res_w, res_h])
        
        if savedebug is not None and isinstance(savedebug, dict):
            savedebug["target_pos_out"] = target_pos.astype(np.float32)
            savedebug["target_sz_out"] = target_sz.astype(np.float32)

        return target_pos, target_sz, cls_score[r_max, c_max], usage_time

    def grids(self, p):
        """
        each element of feature map on input search image
        :return: H*W*2 (position for each element)
        """
        # print('ATTENTION',p.instance_size,p.score_size)
        sz = p.score_size

        # the real shift is -param['shifts']
        sz_x = sz // 2
        sz_y = sz // 2

        x, y = np.meshgrid(np.arange(0, sz) - np.floor(float(sz_x)),
                           np.arange(0, sz) - np.floor(float(sz_y)))

        print(p.total_stride, p.instance_size)
        self.grid_to_search_x = x * p.total_stride + p.instance_size // 2
        self.grid_to_search_y = y * p.total_stride + p.instance_size // 2

    def change(self, r):
        return np.maximum(r, 1. / r)

    def sz(self, w, h):
        pad = (w + h) * 0.5
        sz2 = (w + pad) * (h + pad)
        return np.sqrt(sz2)

    def sz_wh(self, wh):
        pad = (wh[0] + wh[1]) * 0.5
        sz2 = (wh[0] + pad) * (wh[1] + pad)
        return np.sqrt(sz2)

    @staticmethod
    def sigmoid(z):
        """ this function implements the sigmoid function, and
        expects a numpy array as argument """
        if not isinstance(z, np.ndarray):
            raise TypeError
        sigmoid = 1.0 / (1.0 + np.exp(-z))
        return sigmoid
