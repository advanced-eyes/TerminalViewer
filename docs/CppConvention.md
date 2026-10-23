# C++ Code Convention

- Follow the next rules

## 1. Common Naming Rules

- Use pascal case

### 2. Naming Rules

#### 2.1. Variables Nameing Rule

- Please prefix variable names with a type-based keyword
- Except parameters

| Type                      | Prefix | Example        | Member Variable  |
| ------------------------- | ------ | -------------- | ---------------- |
| `bool`                    | `b`    | `bEnabled`     | `m_bEnabled`     |
| `char`                    | `c`    | `cValue`       | `m_cValue`       |
| `short`                   | `s`    | `sCount`       | `m_sCount`       |
| `int`                     | `i`    | `iCount`       | `m_iCount`       |
| `long`                    | `l`    | `lValue`       | `m_lValue`       |
| `long long`               | `ll`   | `llTimestamp`  | `m_llTimestamp`  |
| `float`                   | `f`    | `fRatio`       | `m_fRatio`       |
| `double`                  | `d`    | `dDistance`    | `m_dDistance`    |
| `std::string`             | `str`  | `strName`      | `m_strName`      |
| `std::vector`             | `vec`  | `vecPoints`    | `m_vecPoints`    |
| `std::array`              | `arr`  | `arrValues`    | `m_arrValues`    |
| `std::list`               | `lst`  | `lstItems`     | `m_lstItems`     |
| `std::deque`              | `deq`  | `deqQueue`     | `m_deqQueue`     |
| `std::set`                | `set`  | `setIds`       | `m_setIds`       |
| `std::unordered_set`      | `uset` | `usetIds`      | `m_usetIds`      |
| `std::map`                | `map`  | `mapNames`     | `m_mapNames`     |
| `std::unordered_map`      | `hash` | `hashNames`    | `m_hashNames`    |
| `std::pair`               | `pair` | `pairResult`   | `m_pairResult`   |
| `std::tuple`              | `tup`  | `tupResult`    | `m_tupResult`    |
| Pointer                   | `p`    | `pCamera`      | `m_pCamera`      |
| `std::unique_ptr`         | `up`   | `upCamera`     | `m_upCamera`     |
| `std::shared_ptr`         | `sp`   | `spCamera`     | `m_spCamera`     |
| `std::weak_ptr`           | `wp`   | `wpCamera`     | `m_wpCamera`     |
| Iterator                  | `it`   | `itBegin`      | `m_itBegin`      |
| `std::optional`           | `opt`  | `optValue`     | `m_optValue`     |
| `std::function`           | `func` | `funcCallback` | `m_funcCallback` |
| `std::thread`             | `th`   | `thWorker`     | `m_thWorker`     |
| `std::mutex`              | `mtx`  | `mtxData`      | `m_mtxData`      |
| `std::condition_variable` | `cv`   | `cvReady`      | `m_cvReady`      |
| `std::queue`              | `que`  | `queTasks`     | `m_queTasks`     |
| `std::stack`              | `stk`  | `stkItems`     | `m_stkItems`     |

```c++
// Local variables
void TestFunction()
{
    int iSomeValue = 10;
    std::vector<int> vecSomeValues;
}

// Parameters
void TestFunction(int someValue)
{
    int iSomeValue = someValue;
}
```

#### 2.2. Class memeber variables

- Please follow the naming convection of prefixing class member variables with `m_`

```c++
class SomeClass
{
    ...

private:
    int m_iSomeValue;
    std::vector<int> m_vecSomeValues;
    std::string m_strSomeString;
};
```

#### 2.3. Enum Naming Rules

- Use PascalCase for enum types and start with "E" alphabet.
- Use upper case for enum values.

```c++
enum class EScannerType
{
    STRUCTURE,
    CONFOCAL,
}
```

#### 2.4. Function Naming Rules

- Use PascalCase for function names.
- Function names should start with a verb.
- Use descriptive names that clearly describe the function's behavior.

```c++
bool IsNetworkHasUpscaleLayer();
void SetNumberOfLayers(int numberOfLayers);
int GetNumberOfLayers();
```

- Please access private variables throigh Getter/Setter methods.

```c++
int GetNumberOfLayers()
{
    return getImpl().m_iNumberOfLayers;
}
void SetNumberOfLayers(int numberOfLayers)
{
    getImpl().m_iNumberOfLayers = numberOfLayers;
}
```

### 5. Class

### 5.1. Constructor and Destructor Rules

- Initialize member variables using the member initializer list.
- Do not initialize member variables inside the constructor body unless necessary.

```cpp
SomeClass::SomeClass()
    : m_iSomeValue(0)
    , m_vecSomeValues()
    , m_strSomeString()
{
}
```

#### 5.2. Layout

1. Constructor / Destructor
2. Public functions
3. Getter / Setter
4. Protected functions
5. Private functions
6. Member variables

```c++
class SomeClass
{
public:
    SomeClass();
    ~SomeClass();

public:
    void Function1();
    void Function2();

public:
    int GetSomeValue();
    void SetSomeValue(int someValue);

protected:
    void SomeProtectedFunction();

private:
    int m_iSomeValue;
};
```

### 6. const Rules

- Use `const` whenever a variable or parameter is not modified.
- Use `const` reference for large objects that are read-only.
- Mark member functions as `const` when they do not modify the object.

```cpp
void Process(const std::vector<Point>& vecPoints);
int GetCount() const;
const std::string& GetName() const;
```

### 7. `auto` Rules

- Do not use `auto` when the type is simple and obvious.
- Use `auto` when the type is verbose or implementation-dependent.
- Iterators may use `auto`.

```cpp
int iCount = 0;
std::string strName;

auto it = m_hashCameras.find(iCameraId);
auto spCamera = std::make_shared<Camera>();
for(auto& it: m_hashCameras) {
    // TODO
}
```

### 8. Memory Management Rules

- Prefer RAII.
- Prefer `std::unique_ptr` for exclusive ownership.
- Use `std::shared_ptr` only when shared ownership is required.
- Use raw pointers for non-owning references when appropriate.
- Avoid manual `new` and `delete`.

### 9. Include Rules

- Include only the headers that are required.
- Do not add unnecessary includes.
- Prefer forward declarations when appropriate.
- Use the corresponding header first.
- Use `<>` for external libraries and standard libraries.
- Use `""` for headers belonging to the current project.

Include order:

1. Corresponding header
2. Current project headers
3. External libraries
4. Standard libraries

```cpp
#include "SomeClass.h"

#include "Camera/Camera.h"
#include "Common/Types.h"

#include <QImage>
#include <QTimer>

#include <string>
#include <vector>
#include <unordered_map>
```
