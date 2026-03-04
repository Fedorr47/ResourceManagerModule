                        else if constexpr (std::is_same_v<T, CommandSetState>)
                        {
                            curState = cmd.state;
                        }
                        else if constexpr (std::is_same_v<T, CommandSetStencilRef>)
                        {
                            cmdList_->OMSetStencilRef(static_cast<UINT>(cmd.ref & 0xFFu));
                         }
                        else if constexpr (std::is_same_v<T, CommandSetPrimitiveTopology>)
                        {
                            currentTopology = ToD3DTopology(cmd.topology);
                        }
                        else if constexpr (std::is_same_v<T, CommandBindPipeline>)
                        {
                            curPipe = cmd.pso;
                        }
                        else if constexpr (std::is_same_v<T, CommandBindInputLayout>)
                        {
                            curLayout = cmd.layout;
                        }
                        else if constexpr (std::is_same_v<T, CommandBindVertexBuffer>)
                        {
                            if (cmd.slot >= kMaxVBSlots)
                            {
                                throw std::runtime_error("DX12: BindVertexBuffer: slot out of range");
                            }
                            vertexBuffers[cmd.slot] = cmd.buffer;
                            vbStrides[cmd.slot] = cmd.strideBytes;
                            vbOffsets[cmd.slot] = cmd.offsetBytes;
                        }
                        else if constexpr (std::is_same_v<T, CommandBindIndexBuffer>)
                        {
                            indexBuffer = cmd.buffer;
                            ibType = cmd.indexType;
                            ibOffset = cmd.offsetBytes;
                        }
                        else if constexpr (std::is_same_v<T, CommnadBindTexture2D>)
                        {
                            if (cmd.slot < boundTex.size())
                            {
                                auto it = textures_.find(cmd.texture.id);
                                if (it == textures_.end())
                                {
                                    throw std::runtime_error("DX12: BindTexture2D: texture not found in textures_ map");
                                }

                                if (!it->second.hasSRV)
                                {
                                    throw std::runtime_error("DX12: BindTexture2D: texture has no SRV");
                                }

                                TransitionTexture(cmd.texture, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
                                boundTex[cmd.slot] = GetTextureSRV(cmd.texture);
                            }
                        }
                        else if constexpr (std::is_same_v<T, CommandBindTextureCube>)
                        {
                            if (cmd.slot < boundTex.size())
                            {
                                auto it = textures_.find(cmd.texture.id);
                                if (it == textures_.end())
                                {
                                    throw std::runtime_error("DX12: BindTextureCube: texture not found in textures_ map");
                                }

                                if (!it->second.hasSRV)
                                {
                                    throw std::runtime_error("DX12: BindTextureCube: texture has no SRV");
                                }

                                TransitionTexture(cmd.texture, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
                                boundTex[cmd.slot] = GetTextureSRV(cmd.texture);
                            }
                        }
                        else if constexpr (std::is_same_v<T, CommandTextureDesc>)
                        {
                            if (cmd.slot < boundTex.size())
                            {
                                const UINT idx = static_cast<UINT>(cmd.texture);
                                if (idx == 0 || idx >= kSrvHeapNumDescriptors)
                                {
                                    // null SRV
                                    boundTex[cmd.slot] = srvHeap_->GetGPUDescriptorHandleForHeapStart();
                                    return;
                                }

                                // Transition based on descriptor mapping (keeps barriers correct).
                                TextureHandle handle = ResolveTextureHandleFromDesc(cmd.texture);
                                if (handle)
                                {
                                    TransitionTexture(handle, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
                                }

                                // TextureDescIndex is a real SRV heap index.
                                D3D12_GPU_DESCRIPTOR_HANDLE gpu = srvHeap_->GetGPUDescriptorHandleForHeapStart();
                                gpu.ptr += static_cast<UINT64>(idx) * static_cast<UINT64>(srvInc_);
                                boundTex[cmd.slot] = gpu;
                            }
                        }
                        else if constexpr (std::is_same_v<T, CommandBindStructuredBufferSRV>)
                        {
                            if (cmd.slot < boundTex.size())
                            {
                                boundTex[cmd.slot] = GetBufferSRV(cmd.buffer);
                            }
                            }
                        else if constexpr (std::is_same_v<T, CommandSetUniformInt> ||
                            std::is_same_v<T, CommandUniformFloat4> ||
                            std::is_same_v<T, CommandUniformMat4>)
                            {
                                // DX12 backend does not interpret the name-based uniform commands.
                                // Use CommandSetConstants instead.
                                }
                        else if constexpr (std::is_same_v<T, CommandSetConstants>)
                        {
                            perDrawSlot = cmd.slot;
                            perDrawSize = cmd.size;
                            if (perDrawSize > kMaxPerDrawConstantsBytes)
                            {
                                perDrawSize = kMaxPerDrawConstantsBytes;
                            }
                            if (perDrawSize != 0)
                            {
                                std::memcpy(perDrawBytes.data(), cmd.data.data(), perDrawSize);
                            }
                        }