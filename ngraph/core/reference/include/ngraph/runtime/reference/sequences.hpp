//*****************************************************************************
// Copyright 2020 Intel Corporation
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
//*****************************************************************************

#pragma once

#include <cmath>
#include <ngraph/runtime/reference/concat.hpp>
#include <ngraph/runtime/reference/gru_cell.hpp>
#include <ngraph/runtime/reference/lstm_cell.hpp>
#include <ngraph/runtime/reference/rnn_cell.hpp>
#include <ngraph/runtime/reference/split.hpp>

namespace ngraph
{
    namespace runtime
    {
        namespace reference
        {
            enum class CellType
            {
                RNN,
                GRU,
                LSTM,
            };

            struct CellArgs
            {
                std::string activation_f;         // RNN
                std::string activation_g;         // RNN/GRU
                std::string activation_h;         // RNN/GRU/LSTM
                float clip;                       // RNN/GRU/LSTM
                bool linear_before_reset = false; // GRU
            };

            template <typename T>
            void cell_pass(CellType type,
                           const std::vector<const char*>& inputs,
                           const std::vector<Shape>& shapes,
                           const std::vector<char*>& outputs,
                           const CellArgs& args,
                           bool is_reverse)
            {
                auto squeeze_axis = [](const Shape& shape, size_t axis) -> Shape {
                    Shape new_shape(shape.size() - 1);
                    for (size_t i = 0, j = 0; i < shape.size(); ++i)
                    {
                        if (i != axis)
                        {
                            new_shape[j] = shape[i];
                            j++;
                        }
                    }
                    return new_shape;
                };

                size_t x_shape_size = ngraph::shape_size(shapes[0]);

                // split X
                size_t num_splits = shapes[0].at(1);
                std::vector<std::vector<char>> in_seqs(
                    num_splits, std::vector<char>(x_shape_size / num_splits * sizeof(T)));
                std::vector<char*> pointers(num_splits);
                for (size_t i = 0; i < num_splits; ++i)
                    pointers[is_reverse ? num_splits - i - 1 : i] = in_seqs[i].data();
                reference::split(inputs[0], shapes[0], sizeof(T), 1, num_splits, pointers.data());

                Shape part_shape{shapes[0][0], 1, shapes[2][2]};
                size_t part_shape_size = ngraph::shape_size(part_shape);
                std::vector<std::vector<char>> h_list(
                    num_splits, std::vector<char>(ngraph::shape_size(part_shape) * sizeof(T)));

                // use outputs as a buffer for temporarily values
                char* H_i = outputs[1];
                std::memcpy(H_i, inputs[2], ngraph::shape_size(shapes[2]) * sizeof(T));

                char* C_i = nullptr; // LSTMCell only
                if (type == CellType::LSTM)
                {
                    C_i = outputs[2];
                    std::memcpy(C_i, inputs[3], ngraph::shape_size(shapes[3]) * sizeof(T));
                }

                for (size_t time_step = 0; time_step < num_splits; ++time_step)
                {
                    if (type == CellType::LSTM)
                    {
                        runtime::reference::lstm_cell<T>(
                            reinterpret_cast<const T*>(in_seqs[time_step].data()),
                            squeeze_axis(shapes[0], 1),
                            reinterpret_cast<const T*>(H_i),
                            squeeze_axis(shapes[2], 1),
                            reinterpret_cast<const T*>(C_i),
                            squeeze_axis(shapes[3], 1),
                            reinterpret_cast<const T*>(inputs[4]),
                            squeeze_axis(shapes[4], 0),
                            reinterpret_cast<const T*>(inputs[5]),
                            squeeze_axis(shapes[5], 0),
                            reinterpret_cast<const T*>(inputs[6]),
                            squeeze_axis(shapes[6], 0),
                            reinterpret_cast<T*>(outputs[1]),
                            reinterpret_cast<T*>(outputs[2]),
                            args.activation_f,
                            args.activation_g,
                            args.activation_h,
                            args.clip);
                    }
                    else if (type == CellType::RNN)
                    {
                        runtime::reference::rnn_cell<T>(
                            reinterpret_cast<const T*>(in_seqs[time_step].data()),
                            squeeze_axis(shapes[0], 1),
                            reinterpret_cast<const T*>(H_i),
                            squeeze_axis(shapes[2], 1),
                            reinterpret_cast<const T*>(inputs[3]),
                            squeeze_axis(shapes[3], 0),
                            reinterpret_cast<const T*>(inputs[4]),
                            squeeze_axis(shapes[4], 0),
                            reinterpret_cast<const T*>(inputs[5]),
                            squeeze_axis(shapes[5], 0),
                            reinterpret_cast<T*>(outputs[1]),
                            args.activation_f,
                            args.clip);
                    }
                    else if (type == CellType::GRU)
                    {
                        runtime::reference::gru_cell<T>(
                            reinterpret_cast<const T*>(in_seqs[time_step].data()),
                            squeeze_axis(shapes[0], 1),
                            reinterpret_cast<const T*>(H_i),
                            squeeze_axis(shapes[2], 1),
                            reinterpret_cast<const T*>(inputs[3]),
                            squeeze_axis(shapes[3], 0),
                            reinterpret_cast<const T*>(inputs[4]),
                            squeeze_axis(shapes[4], 0),
                            reinterpret_cast<const T*>(inputs[5]),
                            squeeze_axis(shapes[5], 0),
                            reinterpret_cast<T*>(outputs[1]),
                            args.activation_f,
                            args.activation_g,
                            args.clip,
                            args.linear_before_reset);
                    }
                    std::memcpy(h_list[time_step].data(), outputs[1], part_shape_size * sizeof(T));
                }
                // The tensor that concats all the intermediate output values of the hidden.
                // It has shape [batch_size, seq_length, hidden_size]
                std::vector<Shape> in_shapes(num_splits, part_shape);
                std::vector<const char*> to_concat_pointers(num_splits);
                for (size_t i = 0; i < num_splits; ++i)
                    to_concat_pointers[is_reverse ? num_splits - i - 1 : i] = h_list[i].data();
                runtime::reference::concat(to_concat_pointers,
                                           outputs[0],
                                           in_shapes,
                                           {shapes[0][0], shapes[0][1], shapes[2][2]},
                                           1,
                                           sizeof(T));
            }

            template <typename T>
            void lstm_sequence(const char* X,
                               const Shape& X_shape,
                               const char* H,
                               const Shape& H_shape,
                               const char* C,
                               const Shape& C_shape,
                               const char* seq_lengths,
                               const Shape& seq_lengths_shape,
                               const char* W,
                               const Shape& W_shape,
                               const char* R,
                               const Shape& R_shape,
                               const char* B,
                               const Shape& B_shape,
                               char* Y,
                               char* Ho,
                               char* Co,
                               const std::string& activation_f,
                               const std::string& activation_g,
                               const std::string& activation_h,
                               float clip,
                               op::RecurrentSequenceDirection direction)
            {
                OutputVector results;
                if (direction == op::RecurrentSequenceDirection::FORWARD ||
                    direction == op::RecurrentSequenceDirection::REVERSE)
                {
                    CellArgs args;
                    args.activation_f = activation_f;
                    args.activation_g = activation_g;
                    args.activation_h = activation_h;
                    args.clip = clip;
                    std::vector<const char*> inputs = {X, seq_lengths, H, C, W, R, B};
                    std::vector<char*> outputs = {Y, Ho, Co};
                    std::vector<Shape> shapes = {
                        X_shape, seq_lengths_shape, H_shape, C_shape, W_shape, R_shape, B_shape};
                    cell_pass<T>(CellType::LSTM,
                                 inputs,
                                 shapes,
                                 outputs,
                                 args,
                                 direction == op::RecurrentSequenceDirection::REVERSE);
                }
                else if (direction == op::RecurrentSequenceDirection::BIDIRECTIONAL)
                {
                    // Split bidirectional case to forward + reverse passes.
                    // split inputs
                    std::vector<std::vector<char>> H_split(
                        2, std::vector<char>(sizeof(T) * ngraph::shape_size(H_shape) / 2));
                    std::vector<std::vector<char>> C_split(
                        2, std::vector<char>(sizeof(T) * ngraph::shape_size(C_shape) / 2));
                    std::vector<std::vector<char>> W_split(
                        2, std::vector<char>(sizeof(T) * ngraph::shape_size(W_shape) / 2));
                    std::vector<std::vector<char>> R_split(
                        2, std::vector<char>(sizeof(T) * ngraph::shape_size(R_shape) / 2));
                    std::vector<std::vector<char>> B_split(
                        2, std::vector<char>(sizeof(T) * ngraph::shape_size(B_shape) / 2));
                    char* h_pointers[2] = {H_split[0].data(), H_split[1].data()};
                    char* c_pointers[2] = {C_split[0].data(), C_split[1].data()};
                    char* w_pointers[2] = {W_split[0].data(), W_split[1].data()};
                    char* r_pointers[2] = {R_split[0].data(), R_split[1].data()};
                    char* b_pointers[2] = {B_split[0].data(), B_split[1].data()};
                    reference::split(H, H_shape, sizeof(T), 1, 2, h_pointers);
                    reference::split(C, C_shape, sizeof(T), 1, 2, c_pointers);
                    reference::split(W, W_shape, sizeof(T), 0, 2, w_pointers);
                    reference::split(R, R_shape, sizeof(T), 0, 2, r_pointers);
                    reference::split(B, B_shape, sizeof(T), 0, 2, b_pointers);
                    std::vector<char> forward_res_y(sizeof(T) * H_shape[0] * H_shape[2] *
                                                    X_shape[1]);
                    std::vector<char> reverse_res_y(sizeof(T) * H_shape[0] * H_shape[2] *
                                                    X_shape[1]);
                    std::vector<std::vector<char>> forward_res(
                        2, std::vector<char>(sizeof(T) * H_shape[0] * H_shape[2]));
                    std::vector<std::vector<char>> reverse_res(
                        2, std::vector<char>(sizeof(T) * H_shape[0] * H_shape[2]));

                    CellArgs args;
                    args.activation_f = activation_f;
                    args.activation_g = activation_g;
                    args.activation_h = activation_h;
                    args.clip = clip;
                    std::vector<Shape> shapes = {
                        X_shape, seq_lengths_shape, H_shape, C_shape, W_shape, R_shape, B_shape};
                    // update H,C,W,R,B shapes after split
                    shapes[2][1] = 1;
                    shapes[3][1] = 1;
                    for (int i = 4; i < shapes.size(); ++i)
                    {
                        shapes[i][0] = 1;
                    }
                    // forward pass
                    cell_pass<T>(
                        CellType::LSTM,
                        {X,
                         seq_lengths,
                         h_pointers[0],
                         c_pointers[0],
                         w_pointers[0],
                         r_pointers[0],
                         b_pointers[0]},
                        shapes,
                        {forward_res_y.data(), forward_res[0].data(), forward_res[1].data()},
                        args,
                        false);
                    // reverse pass
                    cell_pass<T>(
                        CellType::LSTM,
                        {X,
                         seq_lengths,
                         h_pointers[1],
                         c_pointers[1],
                         w_pointers[1],
                         r_pointers[1],
                         b_pointers[1]},
                        shapes,
                        {reverse_res_y.data(), reverse_res[0].data(), reverse_res[1].data()},
                        args,
                        true);

                    // Stack together respective outputs from both forward and reverse passes.
                    std::vector<Shape> in_shapes_y = {{H_shape[0], 1, X_shape[1], H_shape[2]},
                                                      {H_shape[0], 1, X_shape[1], H_shape[2]}};
                    std::vector<Shape> in_shapes_h_c = {{H_shape[0], 1, H_shape[2]},
                                                        {H_shape[0], 1, H_shape[2]}};
                    Shape output_shape_y{H_shape[0], 2, X_shape[1], H_shape[2]};
                    Shape output_shape_h_c{H_shape[0], 2, H_shape[2]};

                    runtime::reference::concat({forward_res_y.data(), reverse_res_y.data()},
                                               Y,
                                               in_shapes_y,
                                               output_shape_y,
                                               1,
                                               sizeof(T));
                    runtime::reference::concat({forward_res[0].data(), reverse_res[0].data()},
                                               Ho,
                                               in_shapes_h_c,
                                               output_shape_h_c,
                                               1,
                                               sizeof(T));
                    runtime::reference::concat({forward_res[1].data(), reverse_res[1].data()},
                                               Co,
                                               in_shapes_h_c,
                                               output_shape_h_c,
                                               1,
                                               sizeof(T));
                }
            }

            template <typename T>
            void gru_sequence(const char* X,
                              const Shape& X_shape,
                              const char* H,
                              const Shape& H_shape,
                              const char* seq_lengths,
                              const Shape& seq_lengths_shape,
                              const char* W,
                              const Shape& W_shape,
                              const char* R,
                              const Shape& R_shape,
                              const char* B,
                              const Shape& B_shape,
                              char* Y,
                              char* Ho,
                              const std::string& activation_f,
                              const std::string& activation_g,
                              const float clip,
                              const op::RecurrentSequenceDirection direction,
                              const bool linear_before_reset)
            {
                OutputVector results;
                if (direction == op::RecurrentSequenceDirection::FORWARD ||
                    direction == op::RecurrentSequenceDirection::REVERSE)
                {
                    CellArgs args;
                    args.activation_f = activation_f;
                    args.activation_g = activation_g;
                    args.linear_before_reset = linear_before_reset;
                    args.clip = clip;
                    std::vector<const char*> inputs = {X, seq_lengths, H, W, R, B};
                    std::vector<char*> outputs = {Y, Ho};
                    std::vector<Shape> shapes = {
                        X_shape, seq_lengths_shape, H_shape, W_shape, R_shape, B_shape};
                    cell_pass<T>(CellType::GRU,
                                 inputs,
                                 shapes,
                                 outputs,
                                 args,
                                 direction == op::RecurrentSequenceDirection::REVERSE);
                }
                else if (direction == op::RecurrentSequenceDirection::BIDIRECTIONAL)
                {
                    // Split bidirectional case to forward + reverse passes.
                    // split inputs
                    std::vector<std::vector<char>> H_split(
                        2, std::vector<char>(sizeof(T) * ngraph::shape_size(H_shape) / 2));
                    std::vector<std::vector<char>> W_split(
                        2, std::vector<char>(sizeof(T) * ngraph::shape_size(W_shape) / 2));
                    std::vector<std::vector<char>> R_split(
                        2, std::vector<char>(sizeof(T) * ngraph::shape_size(R_shape) / 2));
                    std::vector<std::vector<char>> B_split(
                        2, std::vector<char>(sizeof(T) * ngraph::shape_size(B_shape) / 2));
                    char* h_pointers[2] = {H_split[0].data(), H_split[1].data()};
                    char* w_pointers[2] = {W_split[0].data(), W_split[1].data()};
                    char* r_pointers[2] = {R_split[0].data(), R_split[1].data()};
                    char* b_pointers[2] = {B_split[0].data(), B_split[1].data()};
                    reference::split(H, H_shape, sizeof(T), 1, 2, h_pointers);
                    reference::split(W, W_shape, sizeof(T), 0, 2, w_pointers);
                    reference::split(R, R_shape, sizeof(T), 0, 2, r_pointers);
                    reference::split(B, B_shape, sizeof(T), 0, 2, b_pointers);
                    std::vector<char> forward_res_y(sizeof(T) * H_shape[0] * H_shape[2] *
                                                    X_shape[1]);
                    std::vector<char> forward_res_h(sizeof(T) * H_shape[0] * H_shape[2]);
                    std::vector<char> reverse_res_y(sizeof(T) * H_shape[0] * H_shape[2] *
                                                    X_shape[1]);
                    std::vector<char> reverse_res_h(sizeof(T) * H_shape[0] * H_shape[2]);

                    CellArgs args;
                    args.activation_f = activation_f;
                    args.activation_g = activation_g;
                    args.linear_before_reset = linear_before_reset;
                    args.clip = clip;
                    std::vector<Shape> shapes = {
                        X_shape, seq_lengths_shape, H_shape, W_shape, R_shape, B_shape};
                    // update H,W,R,B shapes after split
                    shapes[2][1] = 1;
                    for (int i = 3; i < shapes.size(); ++i)
                    {
                        shapes[i][0] = 1;
                    }
                    // forward pass
                    cell_pass<T>(CellType::GRU,
                                 {X,
                                  seq_lengths,
                                  h_pointers[0],
                                  w_pointers[0],
                                  r_pointers[0],
                                  b_pointers[0]},
                                 shapes,
                                 {forward_res_y.data(), forward_res_h.data()},
                                 args,
                                 false);
                    // reverse pass
                    cell_pass<T>(CellType::GRU,
                                 {X,
                                  seq_lengths,
                                  h_pointers[1],
                                  w_pointers[1],
                                  r_pointers[1],
                                  b_pointers[1]},
                                 shapes,
                                 {reverse_res_y.data(), reverse_res_h.data()},
                                 args,
                                 true);

                    // Stack together respective outputs from both forward and reverse passes.
                    std::vector<Shape> in_shapes_y = {{H_shape[0], 1, X_shape[1], H_shape[2]},
                                                      {H_shape[0], 1, X_shape[1], H_shape[2]}};
                    std::vector<Shape> in_shapes_h = {{H_shape[0], 1, H_shape[2]},
                                                      {H_shape[0], 1, H_shape[2]}};
                    Shape output_shape_y{H_shape[0], 2, X_shape[1], H_shape[2]};
                    Shape output_shape_h{H_shape[0], 2, H_shape[2]};

                    runtime::reference::concat({forward_res_y.data(), reverse_res_y.data()},
                                               Y,
                                               in_shapes_y,
                                               output_shape_y,
                                               1,
                                               sizeof(T));
                    runtime::reference::concat({forward_res_h.data(), reverse_res_h.data()},
                                               Ho,
                                               in_shapes_h,
                                               output_shape_h,
                                               1,
                                               sizeof(T));
                }
            }

            template <typename T>
            void rnn_sequence(const char* X,
                              const Shape& X_shape,
                              const char* H,
                              const Shape& H_shape,
                              const char* seq_lengths,
                              const Shape& seq_lengths_shape,
                              const char* W,
                              const Shape& W_shape,
                              const char* R,
                              const Shape& R_shape,
                              const char* B,
                              const Shape& B_shape,
                              char* Y,
                              char* Ho,
                              const std::string& activation_f,
                              float clip,
                              const op::RecurrentSequenceDirection direction)
            {
                OutputVector results;
                if (direction == op::RecurrentSequenceDirection::FORWARD ||
                    direction == op::RecurrentSequenceDirection::REVERSE)
                {
                    CellArgs args;
                    args.activation_f = activation_f;
                    args.clip = clip;
                    std::vector<const char*> inputs = {X, seq_lengths, H, W, R, B};
                    std::vector<char*> outputs = {Y, Ho};
                    std::vector<Shape> shapes = {
                        X_shape, seq_lengths_shape, H_shape, W_shape, R_shape, B_shape};
                    cell_pass<T>(CellType::RNN,
                                 inputs,
                                 shapes,
                                 outputs,
                                 args,
                                 direction == op::RecurrentSequenceDirection::REVERSE);
                }
                else if (direction == op::RecurrentSequenceDirection::BIDIRECTIONAL)
                {
                    // Split bidirectional case to forward + reverse passes.
                    // split inputs
                    std::vector<std::vector<char>> H_split(
                        2, std::vector<char>(sizeof(T) * ngraph::shape_size(H_shape) / 2));
                    std::vector<std::vector<char>> W_split(
                        2, std::vector<char>(sizeof(T) * ngraph::shape_size(W_shape) / 2));
                    std::vector<std::vector<char>> R_split(
                        2, std::vector<char>(sizeof(T) * ngraph::shape_size(R_shape) / 2));
                    std::vector<std::vector<char>> B_split(
                        2, std::vector<char>(sizeof(T) * ngraph::shape_size(B_shape) / 2));
                    char* h_pointers[2] = {H_split[0].data(), H_split[1].data()};
                    char* w_pointers[2] = {W_split[0].data(), W_split[1].data()};
                    char* r_pointers[2] = {R_split[0].data(), R_split[1].data()};
                    char* b_pointers[2] = {B_split[0].data(), B_split[1].data()};
                    reference::split(H, H_shape, sizeof(T), 1, 2, h_pointers);
                    reference::split(W, W_shape, sizeof(T), 0, 2, w_pointers);
                    reference::split(R, R_shape, sizeof(T), 0, 2, r_pointers);
                    reference::split(B, B_shape, sizeof(T), 0, 2, b_pointers);
                    std::vector<char> forward_res_y(sizeof(T) * H_shape[0] * H_shape[2] *
                                                    X_shape[1]);
                    std::vector<char> forward_res_h(sizeof(T) * H_shape[0] * H_shape[2]);
                    std::vector<char> reverse_res_y(sizeof(T) * H_shape[0] * H_shape[2] *
                                                    X_shape[1]);
                    std::vector<char> reverse_res_h(sizeof(T) * H_shape[0] * H_shape[2]);

                    CellArgs args;
                    args.activation_f = activation_f;
                    args.clip = clip;
                    std::vector<Shape> shapes = {
                        X_shape, seq_lengths_shape, H_shape, W_shape, R_shape, B_shape};
                    // update H,W,R,B shapes after split
                    shapes[2][1] = 1;
                    for (int i = 3; i < shapes.size(); ++i)
                    {
                        shapes[i][0] = 1;
                    }
                    // forward pass
                    cell_pass<T>(CellType::RNN,
                                 {X,
                                  seq_lengths,
                                  h_pointers[0],
                                  w_pointers[0],
                                  r_pointers[0],
                                  b_pointers[0]},
                                 shapes,
                                 {forward_res_y.data(), forward_res_h.data()},
                                 args,
                                 false);
                    // reverse pass
                    cell_pass<T>(CellType::RNN,
                                 {X,
                                  seq_lengths,
                                  h_pointers[1],
                                  w_pointers[1],
                                  r_pointers[1],
                                  b_pointers[1]},
                                 shapes,
                                 {reverse_res_y.data(), reverse_res_h.data()},
                                 args,
                                 true);

                    // Stack together respective outputs from both forward and reverse passes.
                    std::vector<Shape> in_shapes_y = {{H_shape[0], 1, X_shape[1], H_shape[2]},
                                                      {H_shape[0], 1, X_shape[1], H_shape[2]}};
                    std::vector<Shape> in_shapes_h = {{H_shape[0], 1, H_shape[2]},
                                                      {H_shape[0], 1, H_shape[2]}};
                    Shape output_shape_y{H_shape[0], 2, X_shape[1], H_shape[2]};
                    Shape output_shape_h{H_shape[0], 2, H_shape[2]};

                    runtime::reference::concat({forward_res_y.data(), reverse_res_y.data()},
                                               Y,
                                               in_shapes_y,
                                               output_shape_y,
                                               1,
                                               sizeof(T));
                    runtime::reference::concat({forward_res_h.data(), reverse_res_h.data()},
                                               Ho,
                                               in_shapes_h,
                                               output_shape_h,
                                               1,
                                               sizeof(T));
                }
            }
        }
    }
}