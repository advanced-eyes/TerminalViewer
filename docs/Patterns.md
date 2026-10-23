# 1. Algorithm Pattern

## 1.1. Folder Structure

- Consider both CPU and GPU implementations.
  - Use `GPU.h` and `*CPU.h` to distinguish between them.

```
/Interface
|- IAlgorithm.h
|- IStrategy.h
/Algorithm
/ RuleMatcher
| / Common
| | |- TermVector.h
| | |- TermVector.cpp
| / Strategy
| | |- TermCosineMatchCPU.h
| | |- TermCosineMatchCPU.cpp
| |- RuleMatcher.h
| |- RuleMatcher.cpp
/ FileDecoder
| / Common
| | |- PdfSyntax.h
| | |- PdfSyntax.cpp
| / Strategy
| | |- PdfInfoDecoderCPU.h
| | |- PdfInfoDecoderCPU.cpp
| |- FileDecoder.h
| |- FileDecoder.cpp
```

## 1.2. Based on Strategy Pattern

```cpp
// header 작성 요령
#pragma once

struct BundleAdjustmentInput
{

};

struct BundleAdjustmentOutput
{

};

enum class EBundleAdjustmentType
{
    STRATEGT1 = 0,
    STRATEGT2 = 1,
    STRATEGT3 = 2,
};

class BALM
{
public:
    virtual bool Execute(BundleAdjustmentInput& in, BundleAdjustmentOutput& out);
};

class BundleAdjustment
{
public:
    BundleAdjustment();

    void SetStrategy(EBundleAdjustmentType type);

    bool Execute(BundleAdjustmentInput& in, BundleAdjustmentOutput& out);

private:
    std::unordered_map<EBundleAdjustmentType, std::unique_ptr<EBundleAdjustmentType>> m_hashStrategy;
};
```

## 1.3. CUDA Kernerl Pattern

- Prefix CUDA kernel functions with `Kernel_`
- Prefix functions to execute CUDA kernel with `Launch`

- `.cuh` Files

```c++
// Strategy1GPU.cuh
#pragma once

#include <utility>
#include <vector>

#include <cuda_runtime.h>
#include <device_launch_parameters.h>
#include <cuda_fp16.h>

////////////////////////////////////////////////////////////////////
// Kernels
////////////////////////////////////////////////////////////////////
__global__ void Kernel_AlgorithmA();

////////////////////////////////////////////////////////////////////
// Launcher
////////////////////////////////////////////////////////////////////
void LaunchAlgorithmA(
  CUstream_st* kpStream);
```

- `.cu` Files

```C++

#include "Strategy1GPU.cuh"

////////////////////////////////////////////////////////////////////
// Kernels
////////////////////////////////////////////////////////////////////
__global__ void Kernel_AlgorithmA()
{
  // Kernel Core Login
}

////////////////////////////////////////////////////////////////////
// Launcher
////////////////////////////////////////////////////////////////////
void LaunchAlgorithmA(
  int height,
  int width,
  CUstream_st* kpStream)
{
  dim3 blocks(32, 32);
  dim3 grids((height + blockSeg.x - 1) / blockSeg.x, (width + blockSeg.y - 1) / blockSeg.y);

  Kernel_AlgorithmA << <grids, blocks, 0, kpStream >> > ();
  cudaStreamSynchronize(kpStream);
}
```

# 2. Thread

## 2.1. Code Pattern

```c++
// Header
#pragma once
class Workflow1
{
public:
    explicit Workflow1(CommunicationModule* comModule)
        :m_kpCommModule(comModule)
        , m_bIsRunning(false)
    {

    }

    virtual ~Workflow1();

    void Start();

    void Stop();

    bool IsRunning() const;

protected:
    CommunicationModule* m_kpCommModule;

private:
    std::thread m_thdWorkflowWorker;
    std::atomic<bool> m_bIsRunning;
};
```

```C++
// Source
#include "Interface/Workflow1.h"

void Workflow1::Start()
{
    if (running)
        return;

    running = true;

    thread = std::thread(&Workflow1::Run, this);
}

void Workflow1::Run()
{
    while (running)
    {
        DoSomething();
    }
}

void Workflow1::Stop()
{
    running = false;

    if (thread.joinable())
        thread.join();
}
```

## 2.2. Communication Pattern

- Use a Ring Buffer or `std::queue` for inter-thread communication.
- Use a single shared `CommunicationModule` to manage communication buffers.
- Ensure thread-safe access when buffers are shared between threads.

```C++
#include <array>
#include <condition_variable>
#include <cstddef>
#include <mutex>

template<typename T, size_t Capacity>
class BlockingRingBuffer
{
public:
    void Push(const T& value)
    {
        {
            std::unique_lock<std::mutex> lock(m_mtx);

            m_cvFull.wait(lock, [this]()
            {
                return m_iSize < Capacity;
            });

            m_arrBuffer[m_iTail] = value;
            m_iTail = (m_iTail + 1) % Capacity;
            ++m_iSize;
        }

        m_cvEmpty.notify_one();
    }

    T Pop()
    {
        std::unique_lock<std::mutex> lock(m_mtx);

        m_cvEmpty.wait(lock, [this]()
        {
            return m_iSize > 0;
        });

        T value = m_arrBuffer[m_iHead];

        m_iHead = (m_iHead + 1) % Capacity;
        --m_iSize;

        lock.unlock();

        m_cvFull.notify_one();

        return value;
    }

private:
    std::array<T, Capacity> m_arrBuffer{};

    size_t m_iHead = 0;
    size_t m_iTail = 0;
    size_t m_iSize = 0;

    std::mutex m_mtx;
    std::condition_variable m_cvEmpty;
    std::condition_variable m_cvFull;
};
```

```c++
#include <array>
#include <condition_variable>
#include <cstddef>
#include <mutex>

template<typename T, size_t Capacity>
class OverwriteRingBuffer
{
public:
    void Push(const T& value)
    {
        {
            std::lock_guard<std::mutex> lock(m_mtx);

            m_arrBuffer[m_iTail] = value;
            m_iTail = (m_iTail + 1) % Capacity;

            if (m_iSize < Capacity)
            {
                ++m_iSize;
            }
            else
            {
                m_iHead = (m_iHead + 1) % Capacity;
            }
        }

        m_cv.notify_one();
    }

    T Pop()
    {
        std::unique_lock<std::mutex> lock(m_mtx);

        m_cv.wait(lock, [this]()
        {
            return m_iSize > 0;
        });

        T value = m_arrBuffer[m_iHead];

        m_iHead = (m_iHead + 1) % Capacity;
        --m_iSize;

        return value;
    }

private:
    std::array<T, Capacity> m_arrBuffer{};

    size_t m_iHead = 0;
    size_t m_iTail = 0;
    size_t m_iSize = 0;

    std::mutex m_mtx;
    std::condition_variable m_cv;
};
```

```cpp
template<typename T>
class Channel
{
public:
    void Push(T data)
    {
        {
            std::lock_guard<std::mutex> lock(mutex);
            queue.push(std::move(data));
        }

        cv.notify_one();
    }

    bool Pop(T& data)
    {
        std::unique_lock<std::mutex> lock(mutex);

        cv.wait(lock, [this]()
        {
            return !queue.empty() || stopped;
        });

        if (queue.empty())
            return false;

        data = std::move(queue.front());
        queue.pop();

        return true;
    }

    void Stop()
    {
        {
            std::lock_guard<std::mutex> lock(mutex);
            stopped = true;
        }

        cv.notify_all();
    }

private:
    std::queue<T> queue;
    std::mutex mutex;
    std::condition_variable cv;
    bool stopped = false;
};
```

## 2.3. Communication Module

- Use a single CommunicationModule istance for communication between workflow threads.
- CommunicationModule is owned by main application using `std::unique`
- Pass the raw pointer of CommunicationModule to workflow threads as a non-owning reference.
- Workflow threads must own or delete the CommunicationModule.

```c++
struct CommunicationModule
{
    // call behine the start of workflow threads.
    void Clear();

    std::queue<int> m_queNewFraneID;
    BlockingRingBuffer<128, int> m_vecWorkflow1Result;
    OverwriteRingBuffer<128, int> m_vecWorkflow2Result;
    Channel<int> m_queWorkflow3Result;
};

class Application
{
public:
    Application()
    {
        m_upCommModule = std::make_unique<CommunicationModule>();
        m_kpWorkflow1 = std::make_unique<OneWorkflowThread>(m_upCommModule.get());
        m_kpWorkflow2 = std::make_unique<TwoWorkflowThread>(m_upCommModule.get());
        m_upCommModule->Clear();

        // Start...
        m_kpWorkflow1->Start();
        m_kpWorkflow2->Start();
    }

private:
    std::unique_ptr<CommunicationModule> m_upCommModule;
    std::unique_ptr<OneWorkflowThread> m_kpWorkflow1;
    std::unique_ptr<TwoWorkflowThread> m_kpWorkflow2;
};
```
