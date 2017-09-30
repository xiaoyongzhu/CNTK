//
// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE.md file in the project root for full license information.
//

#include "CNTKToONNX.h"
#include "./core/graph.h"
#include "Utils.h"
#include "Operators.h"

#include <vector>

using namespace CNTK::ONNX;

namespace CNTK
{

//
// A helper function, to reverse any iterable container and return a copy
// of the reversed container.
//
template<typename ItrType>
ItrType reverse(ItrType v)
{
    std::reverse(std::begin(v), std::end(v));
    return v;
}

class CNTKToONNXHelper
{
public:
    //
    // Copy the entire CNTK graph to ONNX graph.
    //
    static void Copy(const FunctionPtr& src, std::unique_ptr<LotusIR::Graph>& dst);

private:
    //
    // Recursively create ONNX nodes corresponding to each CNTK node.
    //
    static LotusIR::Node* CreateNode(const FunctionPtr& src,
                                     std::unique_ptr<LotusIR::Graph>& graph,
                                     std::unordered_map<FunctionPtr, LotusIR::Node*>& functionNodes,
                                     std::unordered_map<Variable, LotusIR::Node*>& variableNodes);

    //
    // Copy the content of NDArrayView to TensorProto, and do the needed
    // convergence.
    //
    static void CopyTensor(const NDArrayViewPtr src, LotusIR::TensorProto& dst);

    //
    // Copy supported attributes from CNTK node to corresponding ONNX node.
    //
    static void CopyAttributes(const FunctionPtr& src, LotusIR::Node* node);

    //
    // Convert Axis object to actual tensor index.
    //
    static int ToIndex(const Axis& axis);

    //
    // Convert NDShape and various std::vector types to TensorShape
    //
    static LotusIR::TensorShapeProto ToTensorShape(const NDShape& shape, bool hasBatchAxis = false);
    static LotusIR::TensorShapeProto ToTensorShape(const std::vector<bool>& shape);
    static LotusIR::TensorShapeProto ToTensorShape(const std::vector<int>& shape);
    static LotusIR::TensorShapeProto ToTensorShape(const std::vector<Axis>& axes);

    //
    // Convert TensorShapeProto, NDShape and various std::vector types to std::vector
    //
    static std::vector<int64_t> ToINTS(const LotusIR::TensorShapeProto& shape);
    static std::vector<int64_t> ToINTS(const NDShape& shape, bool hasBatchAxis = false);
    static std::vector<int64_t> ToINTS(const std::vector<bool>& shape);
    static std::vector<int64_t> ToINTS(const std::vector<int>& shape);
    static std::vector<int64_t> ToINTS(const std::vector<Axis>& axes);

    //
    // Convert data types.
    //
    static LotusIR::TypeProto ToONNXType(DataType dataType);

    //
    // Map CNTK OP names to ONNX OP Names.
    //
    static std::string ToOPName(const FunctionPtr& src);

    //
    // Which input to ignore during converting a CNTK block to a primitive OP in ONNX.
    //
    static bool FilterInput(const FunctionPtr& src, const CNTK::Variable& input, size_t inputIndex);

    //
    // Argument orders between CNTK and ONNX aren't always the same.
    //
    static std::vector<LotusIR::NodeArg> MapInputsOrderToONNX(const FunctionPtr& src, const std::vector<LotusIR::NodeArg>& inputs);
};

std::unique_ptr<LotusIR::Graph> CNTKToONNX::CreateGraph(const FunctionPtr& src)
{
    std::unique_ptr<LotusIR::Graph> dstGraph(new LotusIR::Graph("CNTKGraph", 1, 1, "CNTK"));
    CNTKToONNXHelper::Copy(src, dstGraph);
    LotusIR::Status status = dstGraph->Resolve();
    if (!status.Ok())
        LogicError("Invalid ONNX Graph.");
    return dstGraph;
}

void CNTKToONNXHelper::Copy(const FunctionPtr& src, std::unique_ptr<LotusIR::Graph>& dst)
{
    std::unordered_map<FunctionPtr, LotusIR::Node*> functionNodes;
    std::unordered_map<Variable, LotusIR::Node*> variableNodes;

    //
    // Iterate through each node in CNTK graph and create an equivalent node
    // in ONNX graph.
    //
    CreateNode(src, dst, functionNodes, variableNodes);
}

void CNTKToONNXHelper::CopyTensor(const NDArrayViewPtr src, LotusIR::TensorProto& dst)
{
    auto dataType = src->GetDataType();
    auto srcT = src->Transpose(); // Column major to row major.
    auto srcShape = srcT->Shape();
    auto totalSize = srcShape.TotalSize();

    // This is our own copy so move it to the CPU.
    srcT->ChangeDevice(DeviceDescriptor::CPUDevice());

    switch (dataType)
    {
        case DataType::Float:
        {
            dst.set_data_type(LotusIR::TensorProto_DataType_FLOAT);
            auto data = srcT->DataBuffer<float>();
            for (size_t index = 0; index < totalSize; index++)
                *(dst.mutable_float_data()->Add()) = data[index];

            break;
        }
        case DataType::Double:
        {
            dst.set_data_type(LotusIR::TensorProto_DataType_DOUBLE);
            auto data = srcT->DataBuffer<double>();
            for (size_t index = 0; index < totalSize; index++)
                *(dst.mutable_double_data()->Add()) = data[index];

            break;
        }
        default:
            NOT_IMPLEMENTED;
    }

    for (auto dim : srcShape.Dimensions())
        *(dst.mutable_dims()->Add()) = dim;
}

int CNTKToONNXHelper::ToIndex(const Axis& axis)
{
    if ((axis == Axis::AllAxes()) || (axis == Axis::AllStaticAxes()))
        LogicError("AllAxes and AllStaticAxes are currently not supported.");

    if (axis.IsSequenceAxis())
        LogicError("Sequence axis are currently not supported.");

    if (axis.IsBatchAxis())
        return 0;
 
    return axis.StaticAxisIndex() + 1;
}

LotusIR::TensorShapeProto CNTKToONNXHelper::ToTensorShape(const NDShape& shape, bool hasBatchAxis)
{
    LotusIR::TensorShapeProto newShape;

    if (hasBatchAxis)
        newShape.add_dim()->set_dim_value(-1);

    auto dimensions = reverse(shape.Dimensions());
    for (auto dimension : dimensions)
        newShape.add_dim()->set_dim_value(dimension);

    return newShape;
}

LotusIR::TensorShapeProto CNTKToONNXHelper::ToTensorShape(const std::vector<bool>& shape)
{
    LotusIR::TensorShapeProto newShape;
    auto dimensions = reverse(shape);
    for (auto dimension : dimensions)
        newShape.add_dim()->set_dim_value(dimension ? 1:0);

    return newShape;
}

LotusIR::TensorShapeProto CNTKToONNXHelper::ToTensorShape(const std::vector<int>& shape)
{
    LotusIR::TensorShapeProto newShape;
    auto dimensions = reverse(shape);
    for (auto dimension : dimensions)
        newShape.add_dim()->set_dim_value(dimension);

    return newShape;
}

LotusIR::TensorShapeProto CNTKToONNXHelper::ToTensorShape(const std::vector<Axis>& axes)
{
    std::vector<int> axesValue;
    for (auto axis : axes)
    {
        axesValue.push_back(ToIndex(axis));
    }
    std::sort(axesValue.begin(), axesValue.end());

    LotusIR::TensorShapeProto newShape;
    for (auto dimension : axesValue)
        newShape.add_dim()->set_dim_value(dimension);

    return newShape;
}

std::vector<int64_t> CNTKToONNXHelper::ToINTS(const LotusIR::TensorShapeProto& shape)
{
    std::vector<int64_t> newShape;

    for (int i = 0; i < shape.dim_size(); i++)
        newShape.push_back((int64_t)shape.dim(i).dim_value());

    return newShape;
}

std::vector<int64_t> CNTKToONNXHelper::ToINTS(const NDShape& shape, bool hasBatchAxis)
{
    return ToINTS(ToTensorShape(shape, hasBatchAxis));
}

std::vector<int64_t> CNTKToONNXHelper::ToINTS(const std::vector<bool>& shape)
{
    return ToINTS(ToTensorShape(shape));
}

std::vector<int64_t> CNTKToONNXHelper::ToINTS(const std::vector<int>& shape)
{
    return ToINTS(ToTensorShape(shape));
}

std::vector<int64_t> CNTKToONNXHelper::ToINTS(const std::vector<Axis>& axes)
{
    return ToINTS(ToTensorShape(axes));
}

LotusIR::TypeProto CNTKToONNXHelper::ToONNXType(DataType dataType)
{
    LotusIR::TypeProto type;
    switch (dataType)
    {
    case DataType::Float:
        type.mutable_tensor_type()->set_elem_type(LotusIR::TensorProto_DataType_FLOAT);
        break;
    case DataType::Double:
        type.mutable_tensor_type()->set_elem_type(LotusIR::TensorProto_DataType_DOUBLE);
        break;
    default:
        NOT_IMPLEMENTED;
    }

    return type;
}

std::string CNTKToONNXHelper::ToOPName(const FunctionPtr& src)
{
    auto lookup = Operators::CntkToONNXLookup();
    assert(lookup.count(src->OpName()) != 0);

    std::string opName = ToString(src->OpName());
    if (lookup.count(src->OpName()) == 1)
    {
        auto attributesMap = lookup.find(src->OpName())->second.map;
        opName = attributesMap[src->OpName()];
    }
    else
    {
        // Some nodes map one to many.
        if (src->OpName() == L"Pooling")
        {
            PoolingType poolingType = (PoolingType)src->Attributes()[L"poolingType"].Value<size_t>();
            if (poolingType == PoolingType::Max)
                opName = "MaxPool";
            else
                opName = "AveragePool";
        }
    }

    return opName;
}

bool CNTKToONNXHelper::FilterInput(const FunctionPtr& src, const CNTK::Variable& input, size_t inputIndex)
{
    // In CNTK block functions, they expose all constants inside the block. For block functions that
    // map directly to ONNX OP, we don't care about constanst inside the block.
    if (input.IsConstant())
        return !Operators::IsValidInputs(src->OpName(), inputIndex);
    return false;
}

//
// This is the main horsepower, it navigate CNTK graph recursivley while keep track of all visited nodes and variables, 
// and create the corresponding ONNX graph.
//
LotusIR::Node* CNTKToONNXHelper::CreateNode(const FunctionPtr& src,
                                            std::unique_ptr<LotusIR::Graph>& graph,
                                            std::unordered_map<FunctionPtr, LotusIR::Node*>& functionNodes,
                                            std::unordered_map<Variable, LotusIR::Node*>& variableNodes)
{
    auto iter = functionNodes.find(src);
    if (iter != functionNodes.end())
        return iter->second;

    LotusIR::Node* functionNode = nullptr;
    std::string opName = ToString(src->OpName());

    //
    // If this block node equivalent to a primitive ONNX OP, then treated as such.
    // And just maps its argument to ONNX node.
    //
    if (src->IsBlock() && !Operators::IsSupportedCNTKOP(src->OpName()))
    {
        functionNode = CreateNode(src->BlockRoot(), graph, functionNodes, variableNodes);
    }
    //
    // For compatibility of other framework that support ONNX, we will limit the list of OPs to the one
    // supported by ONNX https://github.com/onnx/onnx/tree/master/onnx/defs.
    //
    else if (Operators::IsSupportedCNTKOP(src->OpName()))
    {
        std::vector<LotusIR::NodeArg> inputs;
        std::vector<LotusIR::NodeArg> outputs;

        for (const auto& output : src->Outputs())
        {
            LotusIR::NodeArg outputArg(ToString(output.Uid()),
                                       ToONNXType(output.GetDataType()),
                                       ToTensorShape(output.Shape()));
            outputs.push_back(outputArg);
        }

        for (size_t inputIndex = 0; inputIndex < src->Inputs().size(); ++inputIndex)
        {
            auto input = src->Inputs()[inputIndex];

            // TODO: why place holder is not input?
            if (input.IsPlaceholder())
                LogicError("Node '%S': Placeholder isn't supported currently.", src->AsString().c_str());

            if (src->IsBlock() && FilterInput(src, input, inputIndex))
                continue;

            LotusIR::NodeArg inputArg(ToString(input.Uid()),
                                      ToONNXType(input.GetDataType()),
                                      ToTensorShape(input.Shape()));

            inputs.push_back(inputArg);

            //
            // Leaf nodes are data entry to the graph and need their own node with only output arg.
            //
            if (input.IsInput() || input.IsParameter() || input.IsConstant())
            {
                if (variableNodes.find(input) == variableNodes.end())
                {
                    std::vector<LotusIR::NodeArg> varInputs;
                    std::vector<LotusIR::NodeArg> varOutputs;

                    varOutputs.push_back({ inputArg });
                    LotusIR::Node* variableNode = nullptr;
                    if (input.IsParameter() || input.IsConstant())
                    {
                        variableNode = graph->AddNode(ToString(input.Uid()), "Constant", "", varInputs, varOutputs);
                        auto srcTensor = input.IsParameter() ? Parameter(input).Value() : Constant(input).Value();
                        
                        LotusIR::TensorProto dstTensor;
                        CopyTensor(srcTensor, dstTensor);

                        variableNode->AddAttribute("value", dstTensor);
                        variableNodes.emplace(input, variableNode);
                    }
                }
            }
            //
            // If this input is output, then it is the ouput of an up stream node. Recursively add all upstream nodes.
            // Pretty much, we are doing DFS.
            //
            else if (input.IsOutput())
                CreateNode(input.Owner(), graph, functionNodes, variableNodes);
        }

        inputs = MapInputsOrderToONNX(src, inputs);
        functionNode = graph->AddNode(ToString(src->Uid()), ToOPName(src), "", inputs, outputs);
        CopyAttributes(src, functionNode);
    }
    else
        LogicError("Node '%S': Unsupported node.", src->AsString().c_str());

    functionNodes.emplace(src, functionNode);
    return functionNode;
}

void CNTKToONNXHelper::CopyAttributes(const FunctionPtr& src, LotusIR::Node* node)
{
    auto lookup = Operators::CntkToONNXLookup();
    assert(lookup.count(src->OpName()) != 0);

    std::string opName = ToString(src->OpName());
    if (lookup.count(src->OpName()) == 1)
    {
        auto attributesMap = lookup.find(src->OpName())->second.map;
        opName = attributesMap[src->OpName()];

        if ((src->OpName() == L"Convolution") || (src->OpName() == L"ConvolutionTranspose"))
        {
            auto kernelShape = (NDShape)src->Attributes()[L"kernelShape"].Value<NDShape>();
            auto strides = (NDShape)src->Attributes()[L"strides"].Value<NDShape>();
            auto autoPadding = AsVector<bool>(src->Attributes()[L"autoPadding"].Value<std::vector<DictionaryValue>>());
            auto dilations = (NDShape)src->Attributes()[L"dilation"].Value<NDShape>();

            node->AddAttribute(attributesMap[L"kernelShape"], ToINTS(kernelShape));
            node->AddAttribute("strides", ToINTS(strides));
            node->AddAttribute("pads", ToINTS(autoPadding));
            node->AddAttribute(attributesMap[L"dilation"], ToINTS(dilations));
            node->AddAttribute("group", (int64_t)1);

            if (src->OpName() == L"ConvolutionTranspose")
            {
                auto outputShape = (NDShape)src->Attributes()[L"outputShape"].Value<NDShape>();
                node->AddAttribute(attributesMap[L"outputShape"], ToINTS(outputShape));
            }
        }
        else if (src->OpName() == L"BatchNormalization")
        {
            auto spatial = (int64_t)((bool)src->Attributes()[L"spatial"].Value<bool>() ? 1 : 0);
            // auto normalizationTimeConstant = (float)src->Attributes()[L"normalizationTimeConstant"].Value<double>();
            // auto blendTimeConstant = (float)src->Attributes()[L"blendTimeConstant"].Value<double>();
            auto epsilon = (float)src->Attributes()[L"epsilon"].Value<double>();

            node->AddAttribute(attributesMap[L"spatial"], spatial);
            node->AddAttribute("is_test", (int64_t)1);
            node->AddAttribute(attributesMap[L"epsilon"], epsilon);
            node->AddAttribute("momentum", 0.9f);
        }
        else if ((src->OpName() == L"LeakyReLU") || (src->OpName() == L"ELU"))
        {
            auto alpha = 0.01f;
            if (src->Attributes().Contains(L"alpha"))
                alpha = (float)src->Attributes()[L"alpha"].Value<double>();
            node->AddAttribute("alpha", 0.01f);
        }
        else if (src->OpName() == L"SELU")
        {
            auto alpha = 1.6732f;
            if (src->Attributes().Contains(L"alpha"))
                alpha = (float)src->Attributes()[L"alpha"].Value<double>();

            auto gamma = 1.0507f;
            if (src->Attributes().Contains(L"gamma"))
                gamma = (float)src->Attributes()[L"gamma"].Value<double>();

            node->AddAttribute("alpha", alpha);
            node->AddAttribute("gamma", gamma);
        }
        else if (src->OpName() == L"Dropout")
        {
            auto dropoutRate = (float)src->Attributes()[L"dropoutRate"].Value<double>();
            node->AddAttribute(attributesMap[L"dropoutRate"], dropoutRate);
            node->AddAttribute("is_test", (int64_t)1);
        }
        else if ((src->OpName() == L"UniformRandom") || (src->OpName() == L"NormalRandom") ||
                 (src->OpName() == L"UniformRandomLike") || (src->OpName() == L"NormalRandomLike"))
        {
            auto randomArgs = AsVector<double>(src->Attributes()[L"randomDistributionArgs"].Value<std::vector<DictionaryValue>>());
            auto seed = (int64_t)src->Attributes()[L"rngSeed"].Value<int>();
            
            if ((src->OpName() == L"UniformRandom") || (src->OpName() == L"UniformRandomLike"))
            {
                node->AddAttribute("low", (float)randomArgs[0]);
                node->AddAttribute("high", (float)randomArgs[1]);
            }
            else
            {
                node->AddAttribute("mean", (float)randomArgs[0]);
                node->AddAttribute("scale", (float)randomArgs[1]);
            }

            node->AddAttribute(attributesMap[L"rngSeed"], seed);
            if ((src->OpName() == L"UniformRandom") || (src->OpName() == L"NormalRandom"))
            {
                auto shape = (NDShape)src->Attributes()[L"newShape"].Value<NDShape>();
                node->AddAttribute(attributesMap[L"newShape"], ToINTS(shape));
            }
        }
        else if ((src->OpName() == L"ReduceMax")  || (src->OpName() == L"ReduceMin")    ||
                 (src->OpName() == L"ReduceSum")  || (src->OpName() == L"ReduceMean")   ||
                 (src->OpName() == L"ReduceProd") || (src->OpName() == L"ReduceLogSum") ||
                 (src->OpName() == L"Argmax")     || (src->OpName() == L"Argmin"))
        {
            auto keepReducedDimensions = (int64_t)((bool)src->Attributes()[L"reductionKeepDimensions"].Value<bool>() ? 1 : 0);
            std::vector<Axis> reductionAxes;
            if (src->Attributes().Contains(L"axisVec"))
                reductionAxes = AsVector<Axis>(src->Attributes()[L"axisVec"].Value<std::vector<DictionaryValue>>());
            else if (src->Attributes().Contains(L"axis"))
                reductionAxes.push_back((Axis)(src->Attributes()[L"axis"].Value<Axis>()));

            node->AddAttribute(attributesMap[L"reductionKeepDimensions"], keepReducedDimensions);
            node->AddAttribute("axes", ToINTS(reductionAxes));
        }
        else if (src->OpName() == L"Transpose")
        {
            std::vector<Axis> perm = AsVector<Axis>(src->Attributes()[L"axisVec"].Value<std::vector<DictionaryValue>>());
            node->AddAttribute(attributesMap[L"axisVec"], ToINTS(perm));
        }
        else if (src->OpName() == L"Reshape")
        {
            auto shape = (NDShape)src->Attributes()[L"newShape"].Value<NDShape>();
            node->AddAttribute(attributesMap[L"newShape"], ToINTS(shape, true));
        }
        else if (src->OpName() == L"Splice")
        {
            Axis axis = (Axis)(src->Attributes()[L"axis"].Value<Axis>());
            node->AddAttribute(attributesMap[L"axis"], (int64_t)ToIndex(axis));
        }
        else if (src->OpName() == L"Slice")
        {
            std::vector<Axis> sliceAxes;
            std::vector<int> beginIndex;
            std::vector<int> endIndex;

            if (src->Attributes().Contains(L"axisVec"))
            {
                sliceAxes = AsVector<Axis>(src->Attributes()[L"axisVec"].Value<std::vector<DictionaryValue>>());
                beginIndex = AsVector<int>(src->Attributes()[L"beginIndexVec"].Value<std::vector<DictionaryValue>>());
                endIndex = AsVector<int>(src->Attributes()[L"endIndexVec"].Value<std::vector<DictionaryValue>>());
            }
            else if (src->Attributes().Contains(L"axis"))
            {
                sliceAxes.push_back((Axis)(src->Attributes()[L"axis"].Value<Axis>()));
                beginIndex.push_back((int)(src->Attributes()[L"beginIndex"].Value<int>()));
                endIndex.push_back((int)(src->Attributes()[L"endIndex"].Value<int>()));
            }

            node->AddAttribute(attributesMap[L"axes"], ToINTS(sliceAxes));
            node->AddAttribute(attributesMap[L"starts"], ToINTS(beginIndex));
            node->AddAttribute(attributesMap[L"ends"], ToINTS(endIndex));
        }
        else if (src->OpName() == L"Softmax")
        {
            Axis axis = Axis(0);
            if (src->Attributes().Contains(L"axis"))
                axis = (Axis)(src->Attributes()[L"axis"].Value<Axis>());
            node->AddAttribute(attributesMap[L"axis"], (int64_t)ToIndex(axis));
        }
    }
    else
    {
        // Some nodes map one to many.
        if (src->OpName() == L"Pooling")
        {
            auto kernelShape = (NDShape)src->Attributes()[L"poolingWindowShape"].Value<NDShape>();
            auto strides = (NDShape)src->Attributes()[L"strides"].Value<NDShape>();
            auto autoPadding = AsVector<bool>(src->Attributes()[L"autoPadding"].Value<std::vector<DictionaryValue>>());

            node->AddAttribute("kernel_shape", ToINTS(kernelShape));
            node->AddAttribute("strides", ToINTS(strides));
            node->AddAttribute("pads", ToINTS(autoPadding));
        }
    }
}

std::vector<LotusIR::NodeArg> CNTKToONNXHelper::MapInputsOrderToONNX(const FunctionPtr& src, const std::vector<LotusIR::NodeArg>& inputs)
{
    if (Operators::HasInputIndexMap(src->OpName()))
    {
        std::vector<LotusIR::NodeArg> orderedInputs;
        std::map<int, LotusIR::NodeArg> orderedInputsMap;
        auto map = Operators::ToONNXInputIndexMap(src->OpName());

        for (size_t inputIndex = 0; inputIndex < inputs.size(); ++inputIndex)
        {
            if (map[inputIndex] >= 0)
                orderedInputsMap.insert(std::pair<int, LotusIR::NodeArg>(map[inputIndex], inputs[inputIndex]));
        }

        for (const auto& item : orderedInputsMap)
            orderedInputs.push_back(item.second);

        return orderedInputs;
    }

    return inputs;
}

}