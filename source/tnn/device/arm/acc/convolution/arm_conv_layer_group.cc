// Tencent is pleased to support the open source community by making TNN available.
//
// Copyright (C) 2020 THL A29 Limited, a Tencent company. All rights reserved.
//
// Licensed under the BSD 3-Clause License (the "License"); you may not use this file except
// in compliance with the License. You may obtain a copy of the License at
//
// https://opensource.org/licenses/BSD-3-Clause
//
// Unless required by applicable law or agreed to in writing, software distributed
// under the License is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
// CONDITIONS OF ANY KIND, either express or implied. See the License for the
// specific language governing permissions and limitations under the License.

#include "tnn/device/arm/acc/convolution/arm_conv_layer_group.h"

#include <memory>

#include "tnn/device/arm/acc/convolution/arm_conv_int8_layer_common.h"
#include "tnn/device/arm/acc/convolution/arm_conv_layer_1x1.h"
#include "tnn/device/arm/acc/convolution/arm_conv_layer_3x3.h"
#include "tnn/device/arm/acc/convolution/arm_conv_layer_common.h"
#include "tnn/interpreter/raw_buffer.h"
#include "tnn/utils/data_type_utils.h"
#include "tnn/utils/dims_vector_utils.h"

namespace TNN_NS {

Status ArmConvLayerGroup::Init(Context *context, LayerParam *param, LayerResource *resource,
                               const std::vector<Blob *> &inputs, const std::vector<Blob *> &outputs) {
    Status ret;
    ConvLayerParam *conv_param  = dynamic_cast<ConvLayerParam *>(param);
    ConvLayerResource *conv_res = dynamic_cast<ConvLayerResource *>(resource);
    std::vector<shared_ptr<LayerResource>> resources;

    CHECK_PARAM_NULL(conv_param);
    CHECK_PARAM_NULL(conv_res);

    RETURN_ON_NEQ(ArmLayerAcc::Init(context, param, resource, inputs, outputs), TNN_OK);

    group_ = conv_param->group;

    for (int g = 0; g < group_; g++) {
        BlobDesc empty_desc;
        group_inputs_.push_back(std::make_shared<Blob>(empty_desc));
        group_outputs_.push_back(std::make_shared<Blob>(empty_desc));
    }

    RETURN_ON_NEQ(SetGroupParam(group_conv_param_), TNN_OK);
    RETURN_ON_NEQ(SplitResource(resources), TNN_OK);
    RETURN_ON_NEQ(SetSplitBlobDesc(inputs[0], group_inputs_), TNN_OK);
    RETURN_ON_NEQ(SetSplitBlobDesc(outputs[0], group_outputs_), TNN_OK);

    for (int g = 0; g < group_; g++) {
        std::vector<Blob *> local_inputs;
        std::vector<Blob *> local_outputs;
        local_inputs.emplace_back(group_inputs_[g].get());
        local_outputs.emplace_back(group_outputs_[g].get());
        std::shared_ptr<ArmLayerAcc> tmp_acc = nullptr;
        if (inputs[0]->GetBlobDesc().data_type == DATA_TYPE_FLOAT) {
            CreateImpFP(local_inputs, local_outputs, group_conv_param_.get(), tmp_acc);
        } else if (inputs[0]->GetBlobDesc().data_type == DATA_TYPE_INT8) {
            CreateImpInt8(local_inputs, local_outputs, group_conv_param_.get(), tmp_acc);
        }
        CHECK_PARAM_NULL(tmp_acc);
        RETURN_ON_NEQ(tmp_acc->Init(context_, group_conv_param_.get(), resources[g].get(), local_inputs, local_outputs),
                      TNN_OK);

        conv_acc_impls_.push_back(tmp_acc);
    }

    return TNN_OK;
}

ArmConvLayerGroup::~ArmConvLayerGroup() {}

/*
get different impl based on conv params
ArmConvInt8LayerCommon always as the last solution
*/
void ArmConvLayerGroup::CreateImpInt8(const std::vector<Blob *> &inputs, const std::vector<Blob *> &outputs,
                                      LayerParam *param, std::shared_ptr<ArmLayerAcc> &conv_acc_impl) {
    if (!dynamic_cast<ArmConvInt8LayerCommon *>(conv_acc_impl.get())) {
        conv_acc_impl = std::make_shared<ArmConvInt8LayerCommon>();
    }
}

/*
get different impl based on conv params
ArmConvLayerCommon always as the last solution
bfp16 impl included in fp impl
*/
void ArmConvLayerGroup::CreateImpFP(const std::vector<Blob *> &inputs, const std::vector<Blob *> &outputs,
                                    LayerParam *param, std::shared_ptr<ArmLayerAcc> &conv_acc_impl) {
    if (ArmConvLayer3x3::isPrefered(dynamic_cast<ConvLayerParam *>(param_), inputs, outputs)) {
        if (!dynamic_cast<ArmConvLayer3x3 *>(conv_acc_impl.get())) {
            conv_acc_impl = std::make_shared<ArmConvLayer3x3>();
        }
    } else if (ArmConvLayer1x1::isPrefered(dynamic_cast<ConvLayerParam *>(param_), inputs, outputs)) {
        if (!dynamic_cast<ArmConvLayer1x1 *>(conv_acc_impl.get())) {
            conv_acc_impl = std::make_shared<ArmConvLayer1x1>();
        }
    }

    if (!conv_acc_impl) {
        conv_acc_impl = std::make_shared<ArmConvLayerCommon>();
    }
}

Status ArmConvLayerGroup::DoForward(const std::vector<Blob *> &inputs, const std::vector<Blob *> &outputs) {
    Status ret;
    RawBuffer input_buf;
    RawBuffer output_buf;

    RETURN_ON_NEQ(SetSplitBlobDesc(inputs[0], group_inputs_), TNN_OK);
    RETURN_ON_NEQ(SetSplitBlobDesc(outputs[0], group_outputs_), TNN_OK);
    RETURN_ON_NEQ(SetSplitBlobHandle(group_inputs_, input_buf), TNN_OK);
    RETURN_ON_NEQ(SetSplitBlobHandle(group_outputs_, output_buf), TNN_OK);

    // step 1 : split inputs to group inputs
    CopyInputSplitBlob(inputs[0]);

    // step 2 : group forward
    if (conv_acc_impls_.size()) {
        for (int i = 0; i < conv_acc_impls_.size(); i++) {
            std::vector<Blob *> local_inputs;
            std::vector<Blob *> local_outputs;
            local_inputs.emplace_back(group_inputs_[i].get());
            local_outputs.emplace_back(group_outputs_[i].get());
            CHECK_PARAM_NULL(conv_acc_impls_[i].get());
            RETURN_ON_NEQ(conv_acc_impls_[i]->DoForward(local_inputs, local_outputs), TNN_OK);
        }
    } else {
        return Status(TNNERR_LAYER_ERR, "conv_acc_impl_ is nil");
    }

    // step 3 : merge group outputs into one
    CopyOutputSplitBlob(outputs[0]);

    return TNN_OK;
}

Status ArmConvLayerGroup::SetGroupParam(std::shared_ptr<LayerParam> &group_param) {
    auto conv_param = new ConvLayerParam();
    CHECK_PARAM_NULL(conv_param);

    *conv_param                = *(dynamic_cast<ConvLayerParam *>(param_));
    conv_param->output_channel = conv_param->output_channel / conv_param->group;
    conv_param->group          = 1;

    group_param = std::shared_ptr<LayerParam>(conv_param);

    return TNN_OK;
}

Status ArmConvLayerGroup::SetSplitBlobDesc(Blob *blob, std::vector<std::shared_ptr<Blob>> &blobs) {
    auto group_desc    = blob->GetBlobDesc();
    group_desc.dims[1] = group_desc.dims[1] / group_;

    for (int g = 0; g < group_; g++) {
        blobs[g]->SetBlobDesc(group_desc);
    }

    return TNN_OK;
}

Status ArmConvLayerGroup::SetSplitBlobHandle(std::vector<std::shared_ptr<Blob>> &blobs, RawBuffer &buf) {
    auto dims  = blobs[0]->GetBlobDesc().dims;
    auto batch = dims[0];

    if (blobs[0]->GetBlobDesc().data_type == DATA_TYPE_FLOAT) {
        auto r_split_data_count_per_batch = ROUND_UP(dims[1], 4) * dims[2] * dims[3];
        RawBuffer temp(group_ * batch * r_split_data_count_per_batch * sizeof(float));

        for (int g = 0; g < group_; g++) {
            BlobHandle handle;
            handle.base =
                reinterpret_cast<void *>((temp.force_to<float *>() + g * r_split_data_count_per_batch * batch));
            handle.bytes_offset = 0;
            blobs[g].get()->SetHandle(handle);
        }

        buf = temp;
    } else {
        return Status(TNNERR_LAYER_ERR, "split int8 resource not supported");
    }

    return TNN_OK;
}

Status ArmConvLayerGroup::CopyInputSplitBlob(Blob *input) {
    // Todo
    auto dims       = input->GetBlobDesc().dims;
    auto group_dims = group_inputs_[0]->GetBlobDesc().dims;
    auto batch      = dims[0];

    if (input->GetBlobDesc().data_type == DATA_TYPE_FLOAT) {
        auto r_split_data_count_per_batch = ROUND_UP(dims[1] / group_, 4) * dims[2] * dims[3];
        auto r_ori_data_count_per_batch   = ROUND_UP(dims[1], 4) * dims[2] * dims[3];

        auto input_origin = reinterpret_cast<float *>(GetBlobHandlePtr(input->GetHandle()));

        for (int b = 0; b < batch; b++) {
            auto input_ptr = input_origin + b * r_ori_data_count_per_batch;
            RawBuffer temp(group_ * r_split_data_count_per_batch * sizeof(float));
            UnpackC4(temp.force_to<float *>(), input_ptr, dims[2] * dims[3], dims[1]);
            for (int g = 0; g < group_; g++) {
                auto group_input_ptr = reinterpret_cast<float *>(GetBlobHandlePtr(group_inputs_[g]->GetHandle()));
                PackC4(group_input_ptr + b * r_split_data_count_per_batch,
                       temp.force_to<float *>() + g * DimsVectorUtils::Count(group_dims, 1, 4),
                       DimsVectorUtils::Count(group_dims, 2, 4), group_dims[1]);
            }
        }
    } else {
        return Status(TNNERR_LAYER_ERR, "split int8 resource not supported");
    }

    return TNN_OK;
}

Status ArmConvLayerGroup::CopyOutputSplitBlob(Blob *output) {
    // Todo
    auto dims       = output->GetBlobDesc().dims;
    auto group_dims = group_outputs_[0]->GetBlobDesc().dims;
    auto batch      = dims[0];

    if (output->GetBlobDesc().data_type == DATA_TYPE_FLOAT) {
        auto r_split_data_count_per_batch = ROUND_UP(dims[1] / group_, 4) * dims[2] * dims[3];
        auto r_ori_data_count_per_batch   = ROUND_UP(dims[1], 4) * dims[2] * dims[3];

        auto output_origin = reinterpret_cast<float *>(GetBlobHandlePtr(output->GetHandle()));

        for (int b = 0; b < batch; b++) {
            auto output_ptr = output_origin + b * r_ori_data_count_per_batch;
            RawBuffer temp(group_ * r_split_data_count_per_batch * sizeof(float));
            for (int g = 0; g < group_; g++) {
                auto group_output_ptr = reinterpret_cast<float *>(GetBlobHandlePtr(group_outputs_[g]->GetHandle()));
                UnpackC4(temp.force_to<float *>() + g * DimsVectorUtils::Count(group_dims, 1, 4),
                         group_output_ptr + b * r_split_data_count_per_batch, DimsVectorUtils::Count(group_dims, 2, 4),
                         group_dims[1]);
            }
            PackC4(output_ptr, temp.force_to<float *>(), dims[2] * dims[3], dims[1]);
        }
    } else {
        return Status(TNNERR_LAYER_ERR, "split int8 resource not supported");
    }

    return TNN_OK;
}

Status ArmConvLayerGroup::SplitResource(std::vector<std::shared_ptr<LayerResource>> &resources) {
    auto conv_param = dynamic_cast<ConvLayerParam *>(param_);
    auto conv_res   = dynamic_cast<ConvLayerResource *>(resource_);

    auto group_filter_bytes_size = conv_res->filter_handle.GetBytesSize() / group_;
    auto origin_filter_ptr       = conv_res->filter_handle.force_to<char *>();

    for (int g = 0; g < group_; g++) {
        auto group_res = new ConvLayerResource();
        if (conv_res->filter_handle.GetDataType() == DATA_TYPE_FLOAT) {
            group_res->filter_handle =
                RawBuffer(group_filter_bytes_size, origin_filter_ptr + g * group_filter_bytes_size);
            if (conv_param->bias) {
                auto group_bias_bytes_size = conv_res->bias_handle.GetBytesSize() / group_;
                auto origin_bias_ptr       = conv_res->bias_handle.force_to<char *>();

                group_res->bias_handle = RawBuffer(group_bias_bytes_size, origin_bias_ptr + g * group_bias_bytes_size);
            }
        } else {
            return Status(TNNERR_LAYER_ERR, "split int8 resource not supported");
        }

        resources.push_back(std::shared_ptr<LayerResource>(group_res));
    }

    return TNN_OK;
}

}  // namespace TNN_NS
