#!/bin/bash
# Copyright 2024-2025, NVIDIA CORPORATION & AFFILIATES. All rights reserved.
#
# Redistribution and use in source and binary forms, with or without
# modification, are permitted provided that the following conditions
# are met:
#  * Redistributions of source code must retain the above copyright
#    notice, this list of conditions and the following disclaimer.
#  * Redistributions in binary form must reproduce the above copyright
#    notice, this list of conditions and the following disclaimer in the
#    documentation and/or other materials provided with the distribution.
#  * Neither the name of NVIDIA CORPORATION nor the names of its
#    contributors may be used to endorse or promote products derived
#    from this software without specific prior written permission.
#
# THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS ``AS IS'' AND ANY
# EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
# IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
# PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL THE COPYRIGHT OWNER OR
# CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
# EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
# PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
# PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY
# OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
# (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
# OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

### Helpers ###

function install_deps() {
    # Install python bindings for tritonserver and tritonfrontend
    # pip install /opt/tritonserver/python/triton*.whl

    # Install application/testing requirements
    pushd openai/
    # NOTE: Should be pre-installed in container, but can uncomment if needed
    # pip install -r requirements.txt
    pip install -r requirements-test.txt

    if [ "${IMAGE_KIND}" == "TRTLLM" ]; then
        prepare_tensorrtllm meta-llama/Meta-Llama-3.1-8B-Instruct tests/tensorrtllm_models /tmp/engines/llama/3.1-8b-instruct/
        prepare_tensorrtllm mistralai/Mistral-Nemo-Instruct-2407 tests/tensorrtllm_mistral_models /tmp/engines/mistral/nemo-instruct-2407/
    else
        prepare_vllm
    fi
    popd
}

function prepare_vllm() {
    echo "No prep needed for vllm currently"
}

function prepare_tensorrtllm() {
    # FIXME: Remove when testing TRT-LLM containers built from source
    pip install -r requirements.txt

    MODEL="$1"
    MODEL_REPO="$2"
    ENGINE_PATH="$3"

    mkdir -p ${MODEL_REPO}
    cp /app/all_models/inflight_batcher_llm/* "${MODEL_REPO}" -r
    # Ensemble model is not needed for the test
    rm -rf ${MODEL_REPO}/ensemble

    # 1. Generate the model's trt engines
    python3 ../generate_engine.py --model "${MODEL}" --engine_path "${ENGINE_PATH}"

    # 2. Prepare model repository
    FILL_TEMPLATE="/app/tools/fill_template.py"
    python3 ${FILL_TEMPLATE} -i ${MODEL_REPO}/preprocessing/config.pbtxt tokenizer_dir:${ENGINE_PATH},triton_max_batch_size:64,preprocessing_instance_count:1,max_queue_size:0
    python3 ${FILL_TEMPLATE} -i ${MODEL_REPO}/postprocessing/config.pbtxt tokenizer_dir:${ENGINE_PATH},triton_max_batch_size:64,postprocessing_instance_count:1
    python3 ${FILL_TEMPLATE} -i ${MODEL_REPO}/tensorrt_llm_bls/config.pbtxt triton_max_batch_size:64,decoupled_mode:True,bls_instance_count:1,accumulate_tokens:False,logits_datatype:TYPE_FP32
    python3 ${FILL_TEMPLATE} -i ${MODEL_REPO}/tensorrt_llm/config.pbtxt triton_backend:tensorrtllm,triton_max_batch_size:64,decoupled_mode:True,max_beam_width:1,engine_dir:${ENGINE_PATH},batching_strategy:inflight_fused_batching,max_queue_size:0,max_queue_delay_microseconds:1000,encoder_input_features_data_type:TYPE_FP16,logits_datatype:TYPE_FP32,exclude_input_in_output:True
}

function pre_test() {
    # Cleanup
    rm -rf openai/
    rm -f *.xml *.log

    # Prep test environment
    cp -r ../../python/openai .
    install_deps
}

function run_test() {
    pushd openai/
    TEST_LOG="test_openai.log"

    # Capture error code without exiting to allow log collection
    set +e
    pytest -s -v --junitxml=test_openai.xml tests/ 2>&1 > ${TEST_LOG}
    if [ $? -ne 0 ]; then
        cat ${TEST_LOG}
        echo -e "\n***\n*** Test Failed\n***"
        RET=1
    fi
    set -e

    if [ "$RET" == "0" ]; then
        # rerun the tool calling tests with mistral model to cover the mistral tool call parser
        set +e
        TEST_TOOL_CALL_PARSER="mistral" TEST_TOKENIZER="mistralai/Mistral-Nemo-Instruct-2407" pytest -s -v --junitxml=test_openai.xml tests/test_tool_calling.py 2>&1 > ${TEST_LOG}
        if [ $? -ne 0 ]; then
            cat ${TEST_LOG}
            echo -e "\n***\n*** Test Failed\n***"
            RET=1
        fi
        set -e
    fi

    # Collect logs for error analysis when needed
    cp *.xml *.log ../../../
    popd
}

### Test ###

RET=0

pre_test
run_test

exit ${RET}
