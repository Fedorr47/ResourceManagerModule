                        else if constexpr (std::is_same_v<T, CommandDrawIndexed>)
                        {
                            // PSO + RootSig
                            ID3D12PipelineState* pso = EnsurePSO(curPipe, curLayout);
                            cmdList_->SetPipelineState(pso);
                            cmdList_->SetGraphicsRootSignature(rootSig_.Get());

                            // IA bindings (slot0..slotN based on input layout)
                            auto layIt = layouts_.find(curLayout.id);
                            if (layIt == layouts_.end())
                            {
                                throw std::runtime_error("DX12: input layout handle not found");
                            }

                            std::uint32_t maxSlot = 0;
                            for (const auto& e : layIt->second.elems)
                            {
                                maxSlot = std::max(maxSlot, static_cast<std::uint32_t>(e.InputSlot));
                            }

                            const std::uint32_t numVB = layIt->second.elems.empty()
                                ? 0u
                                : (maxSlot + 1u);
                            if (numVB > kMaxVBSlots)
                            {
                                throw std::runtime_error("DX12: input layout uses more VB slots than supported");
                            }

                            std::array<D3D12_VERTEX_BUFFER_VIEW, kMaxVBSlots> vbv{};
                            for (std::uint32_t s = 0; s < numVB; ++s)
                            {
                                if (!vertexBuffers[s])
                                {
                                    throw std::runtime_error("DX12: missing vertex buffer binding for required slot");
                                }
                                auto vbIt = buffers_.find(vertexBuffers[s].id);
                                if (vbIt == buffers_.end())
                                {
                                    throw std::runtime_error("DX12: vertex buffer not found");
                                }

                                const std::uint32_t off = vbOffsets[s];
                                vbv[s].BufferLocation = vbIt->second.resource->GetGPUVirtualAddress() + off;
                                vbv[s].SizeInBytes = (UINT)(vbIt->second.desc.sizeInBytes - off);
                                vbv[s].StrideInBytes = vbStrides[s];
                            }
                            cmdList_->IASetVertexBuffers(0, numVB, vbv.data());
                            cmdList_->IASetPrimitiveTopology(currentTopology);

                            if (indexBuffer)
                            {
                                auto ibIt = buffers_.find(indexBuffer.id);
                                if (ibIt == buffers_.end())
                                {
                                    throw std::runtime_error("DX12: index buffer not found");
                                }
                                D3D12_INDEX_BUFFER_VIEW ibv{};
                                ibv.BufferLocation = ibIt->second.resource->GetGPUVirtualAddress() + ibOffset
                                    + static_cast<UINT64>(cmd.firstIndex) * static_cast<UINT64>(IndexSizeBytes(cmd.indexType));
                                ibv.SizeInBytes = static_cast<UINT>(ibIt->second.desc.sizeInBytes - ibOffset);
                                ibv.Format = (cmd.indexType == IndexType::UINT16) ? DXGI_FORMAT_R16_UINT : DXGI_FORMAT_R32_UINT;

                                cmdList_->IASetIndexBuffer(&ibv);
                            }

                            // Root bindings: CBV (0) + SRV table (1)
                            WriteCBAndBind();

                            for (UINT i = 0; i < kMaxSRVSlots; ++i)
                            {
                                cmdList_->SetGraphicsRootDescriptorTable(1 + i, boundTex[i]);
                            }

                            constexpr UINT kBindlessRootParam = 1 + kMaxSRVSlots;
                            cmdList_->SetGraphicsRootDescriptorTable(kBindlessRootParam, srvHeap_->GetGPUDescriptorHandleForHeapStart());
                            cmdList_->DrawIndexedInstanced(cmd.indexCount, cmd.instanceCount, 0, cmd.baseVertex, cmd.firstInstance);
                        }
                        else if constexpr (std::is_same_v<T, CommandDraw>)
                        {
                            ID3D12PipelineState* pso = EnsurePSO(curPipe, curLayout);
                            cmdList_->SetPipelineState(pso);
                            cmdList_->SetGraphicsRootSignature(rootSig_.Get());

                            // IA bindings (slot0..slotN based on input layout)
                            auto layIt = layouts_.find(curLayout.id);
                            if (layIt == layouts_.end())
                            {
                                throw std::runtime_error("DX12: input layout handle not found");
                            }

                            std::uint32_t maxSlot = 0;
                            for (const auto& e : layIt->second.elems)
                            {
                                maxSlot = std::max(maxSlot, static_cast<std::uint32_t>(e.InputSlot));
                            }
                            const std::uint32_t numVB = layIt->second.elems.empty()
                                ? 0u
                                : (maxSlot + 1u);
                            if (numVB > kMaxVBSlots)
                            {
                                throw std::runtime_error("DX12: input layout uses more VB slots than supported");
                            }

                            std::array<D3D12_VERTEX_BUFFER_VIEW, kMaxVBSlots> vbv{};
                            for (std::uint32_t s = 0; s < numVB; ++s)
                            {
                                if (!vertexBuffers[s])
                                {
                                    throw std::runtime_error("DX12: missing vertex buffer binding for required slot");
                                }
                                auto vbIt = buffers_.find(vertexBuffers[s].id);
                                if (vbIt == buffers_.end())
                                {
                                    throw std::runtime_error("DX12: vertex buffer not found");
                                }

                                const std::uint32_t off = vbOffsets[s];
                                vbv[s].BufferLocation = vbIt->second.resource->GetGPUVirtualAddress() + off;
                                vbv[s].SizeInBytes = (UINT)(vbIt->second.desc.sizeInBytes - off);
                                vbv[s].StrideInBytes = vbStrides[s];
                            }
                            cmdList_->IASetVertexBuffers(0, numVB, vbv.data());
                            cmdList_->IASetPrimitiveTopology(currentTopology);

                            WriteCBAndBind();

                            for (UINT i = 0; i < kMaxSRVSlots; ++i)
                            {
                                cmdList_->SetGraphicsRootDescriptorTable(1 + i, boundTex[i]);
                            }

                            constexpr UINT kBindlessRootParam = 1 + kMaxSRVSlots;
                            cmdList_->SetGraphicsRootDescriptorTable(kBindlessRootParam, srvHeap_->GetGPUDescriptorHandleForHeapStart());
                            cmdList_->DrawInstanced(cmd.vertexCount, cmd.instanceCount, cmd.firstVertex, cmd.firstInstance);
                            }
                        else if constexpr (std::is_same_v<T, CommandDX12ImGuiRender>)
                        {
                            if (!imguiInitialized_ || !cmd.drawData)
                            {
                                return;
                            }

                            // Ensure ImGui sees the same shader-visible heap.
                            ID3D12DescriptorHeap* heaps[] = { srvHeap_.Get() };
                            cmdList_->SetDescriptorHeaps(1, heaps);

                            ImGui_ImplDX12_RenderDrawData(reinterpret_cast<ImDrawData*>(const_cast<void*>(cmd.drawData)), cmdList_.Get());
                            }
                        else if constexpr (std::is_same_v<T, CommandBindTexture2DArray>)
                        {
                            if (cmd.slot < boundTex.size())
                            {
                                auto it = textures_.find(cmd.texture.id);
                                if (it == textures_.end())
                                {
                                    throw std::runtime_error("DX12: BindTexture2DArray: texture not found in textures_ map");
                                }

                                // Ensure an Array SRV exists for cube textures.
                                if (!it->second.hasSRVArray)
                                {
                                    const auto desc = it->second.resource->GetDesc();
                                    AllocateSRV_CubeAsArray(it->second, it->second.srvFormat, desc.MipLevels);
                                }

                                TransitionTexture(cmd.texture, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
                                boundTex[cmd.slot] = it->second.srvGpuArray;
                            }
                            }
                        else
                        {
                            // other commands ignored
                        }