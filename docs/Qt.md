# Development Rule

- Qt5로 상용 배포 시 라이센스 비용 문제 없는 어플리케이션 개발
- C++

### 1. QThread Pattern

- Follow the next rules
- One thread, One files

#### 1.1. Header

```c++
// Header
#pragma once

#include <memory>
#include <QObject>

class MainThreadWorker: public QObject {
	Q_OBJECT

public:
	explicit MainThreadWorker(QObject* parent = nullptr);
	~MainThreadWorker();

	void SetContext(HOctWorkflowManager* pWorkMgr);

	void Stop();

	bool IsThreadAlive() const;

public slots:
	void DoWork();

signals:
	void WorkFinished(bool success);

	void ErrorOccurred(int code);

private:
	class WorkerImpl;
	std::unique_ptr<WorkerImpl> m_pimpl;
};

class MainThread : public QObject {
	Q_OBJECT

public:
	explicit MainThread(hios* kpHIOS);

	~MainThread();

	void start();

	void stop();

signals:
	void Finished(bool success);

	void errorDetected(int errorCode);

private:
	class MainThreadImpl;

	std::unique_ptr<MainThreadImpl> m_pimpl;
};
```

#### 1.2. Source

```c++
#include "pch.h"
#include "MainThread.h"
#include "HMemoryDebug.h"

////////////////////////////////////////////////////////////////////////////////
// WorkerImpl
////////////////////////////////////////////////////////////////////////////////
class MainThreadWorker::MainThreadWorkerImpl {
public:
	std::atomic<bool> doRunThread{ false };

	void DoWork();
};

void MainThreadWorker::MainThreadWorkerImpl::DoWork()
{
  // Work
}

////////////////////////////////////////////////////////////////////////////////
// Worker
////////////////////////////////////////////////////////////////////////////////
MainThreadWorker::MainThreadWorker(QObject* parent)
	: QObject(parent)
	, m_pimpl(std::make_unique<MainThreadWorkerImpl>())
{
  // Worker 필요한 리소스 할당
}

HOCTTrackingWorker::~HOCTTrackingWorker()
{
  // 할당 해제
}

void MainThreadWorker::SetContext(HOctWorkflowManager* pWorkMgr)
{
	m_pimpl->pOctWorkManager = pWorkMgr;
}

void MainThreadWorker::Stop()
{
	m_pimpl->doRunThread = false;
}

bool MainThreadWorker::IsThreadAlive() const
{
	return m_pimpl->doRunThread;
}

void MainThreadWorker::DoWork()
{
	do {
        m_pimpl->m_pimpl();

		QCoreApplication::processEvents();
	} while (m_pimpl->doRunThread);

	emit WorkFinished(true);
}

////////////////////////////////////////////////////////////////////////////////
// ThreadImpl
////////////////////////////////////////////////////////////////////////////////
class MainThread::MainThreadImpl {
public:
	QThread workerThread;
	std::unique_ptr<MainThreadWorker, void(*)(QObject*)> worker;

	hios* kpHIOS;
	HOctWorkflowManager* kpOctWorkManager;

	MainThreadImpl(hios* pHIOS) : worker(nullptr, [](QObject* obj) { if (obj) obj->deleteLater(); })
    {
	}

	~MainThreadImpl() {
		stopWorker();
	}

	void connectWorker(HOCTTrackingThread* q_ptr) {
		if (!worker) return;
		QObject::connect(worker.get(), &MainThreadWorker::WorkFinished, q_ptr, &MainThread::Finished);
		QObject::connect(worker.get(), &MainThreadWorker::ErrorOccurred, q_ptr, &MainThread::errorDetected);
	}

	void stopWorker() {
		if (workerThread.isRunning()) {
			if (worker) worker->Stop();
			workerThread.quit();
			if (!workerThread.wait(4000)) {
				workerThread.terminate();
				workerThread.wait();
			}
		}
		worker.reset();
	}
};

////////////////////////////////////////////////////////////////////////////////
// Thread
////////////////////////////////////////////////////////////////////////////////
MainThread::MainThread(hios* kpHIOS)
	: m_pimpl(std::make_unique<MainThreadImpl>(kpHIOS)) {
}

MainThread::~MainThread() = default;

void MainThread::start()
{
	if (!m_pimpl->worker) {
		m_pimpl->worker.reset(new MainThreadWorker());
		m_pimpl->connectWorker(this);
		m_pimpl->kpOctWorkManager = m_pimpl->kpHIOS->m_kpOctWorkManager;
		m_pimpl->worker->SetContext(m_pimpl->kpOctWorkManager);
		m_pimpl->worker->moveToThread(&m_pimpl->workerThread);
	}

	if (!m_pimpl->workerThread.isRunning()) {
		m_pimpl->workerThread.start();
	}

	QMetaObject::invokeMethod(m_pimpl->worker.get(), "DoWork");
}

void MainThread::stop()
{
	m_pimpl->stopWorker();
}

#endif
```

## 2. Application Architecture

- 주요 모듈은 std::unique_ptr로 선언하고, Application 클래스에 정의해둔다.
- 모든 Widget은 아래 모듈을 injection 받고, 이를 사용하여 쇼하이드를 한다.

### 2.1. Communiation between UI

- Observers와 SystemStatus 클래스를 활용하여 UI간 통신을 수행한다.

#### 2.1.1 Observers

- Observer에 `Signal` 생성

```c++

#pragma once

#include <QObject>
#include <QString>

class Observers : public QObject
{
    Q_OBJECT

public:
    explicit Observers(QObject* parent = nullptr)
        : QObject(parent)
    {
    }

signals:
    void messageChanged(const QString& message);
    void valueChanged(int value);
    void requestRefresh();
};

```

- Sender

```C++
class ControlWidget : public QWidget
{
    Q_OBJECT

public:
    explicit ControlWidget(Observers& observers,
                           QWidget* parent = nullptr);

private:
    Observers& m_observers;
};

ControlWidget::ControlWidget(
    Observers& observers,
    QWidget* parent)
    : QWidget(parent)
    , m_observers(observers)
{
    connect(
        ui->spinBox,
        &QSpinBox::valueChanged,
        &m_observers,
        &Observers::valueChanged);
}
```

- Listener

```C++
class ViewWidget : public QWidget
{
    Q_OBJECT

public:
    explicit ViewWidget(Observers& observers,
                        QWidget* parent = nullptr);

private slots:
    void onValueChanged(int value);

private:
    Observers& m_observers;
};

ViewWidget::ViewWidget(
    Observers& observers,
    QWidget* parent)
    : QWidget(parent)
    , m_observers(observers)
{
    connect(
        &m_observers,
        &Observers::valueChanged,
        this,
        &ViewWidget::onValueChanged);
}

void ViewWidget::onValueChanged(int value)
{
    ui->label->setText(QString::number(value));
}
```

### 2.1.2. SystemStatus

- Application이 가지고 있어얄 상태값을 보관하는 클래스이다.

### 2.2. UI Render Pattern

#### 2.2.1. UI 분기 로직

- 각 컴포넌트는 SystemStatus를 통해 내부 show, hide 함수에서 UI 분기 코드를 작성한다.

```C++

class SomeWidget: public QWidget
{
public:
    SomeWidget(Observers* kpObserver, SystemStatus* kpSystem);

public:
    virtual show() override
    {
        if (m_kpSyste->IsKnowledgetMode())
        {
            // TODO:
        }
        else
        {
            hide();
        }
    }

private:
    SystemStatus* m_kpSyste;
    Observers* m_kpObserver;
}

```

#### 2.2.2. UI 이벤트 처리

- 여러개의 컨트롤이 있을 때, 이벤트 처리 함수를 한 곳에서 처리한다.

```c++
ui->pushButtonA->installEventFilter(this);
ui->pushButtonB->installEventFilter(this);
ui->pushButtonC->installEventFilter(this);
```

```c++
bool MyWidget::eventFilter(QObject* watched, QEvent* event)
{
    if (event->type() == QEvent::MouseButtonPress)
    {
					if (watched == ui->pushButtonA)
					{
							// TODO
					}
					else if (watched == ui->pushButtonB)
					{
							// TODO
					}
    }

    return QWidget::eventFilter(watched, event);
}
```

## 3. UI Architecture

- 간편한 UI 교체를 위해 비지니스 로직과 뷰 로직을 분리한다.
- `ui`파일을 활용하여 UI 개발을 진행한다.
- 스타일은 `qss`를 기반으로 디자인한다.

### 3.1. Folder Structure

```
src/
├─ main.cpp
├─ Core/          앱 조립 · 통신 · 상태 (Application, Observers, SystemStatus, Types, *Service)
├─ Interface/     추상 인터페이스
├─ Algorithm/     알고리즘 (Patterns.md §1)
├─ Document/      문서 · 저장소
├─ Backup/        백업
├─ Thread/        작업 스레드 (§1)
├─ Presenter/     화면 로직. 화면 하나에 Presenter 하나 (위젯을 모른다)
│  ├─ Learn/      LearnPresenter
│  └─ Structure/  StructurePresenter
└─ UI/            뷰. 그리기 · 입력 · 배치만
   ├─ Common/     모든 시안이 함께 쓰는 부품 (OceanWidget, ModalDialog, Shortcuts, Control/, Theme/, Docking/, Terminal/)
   ├─ View/       시안이 골라 쓰는 화면 · 대화상자 (Learn/, Structure/, Shell/, Knowledge/, Rule/)
   └─ Skin/       UI 시안
      ├─ ISkin.h, SkinWindow.h, SkinRegistry.h/.cpp
      ├─ Default/ DefaultSkin, MainWindow(.ui), Default.qss, Default.qrc
      └─ Compact/ CompactSkin, CompactWindow(.ui), CompactLearnView(.ui), Compact.qss, Compact.qrc
resources/style/
└─ Ocean.qss      공통 스타일. 시안 qss 가 그 뒤에 붙어 덮어쓴다
```

의존은 위쪽으로만 한다: `UI/Skin` → `UI/View` → `UI/Common` → `Presenter` → 비즈니스.

| 층 | 들어가는 것 | include 해도 되는 것 |
| --- | --- | --- |
| 비즈니스 (Core, Interface, Algorithm, Document, Backup, Thread) | 데이터 · 규칙 · 저장 · 계산 | 비즈니스끼리만. `Presenter/`, `UI/`, QtWidgets 는 안 된다(조립 지점 `Application.cpp`, `main.cpp` 만 예외) |
| Presenter | 화면에 보일 값 만들기, 사용자 동작 처리 | 비즈니스. `UI/`, QtWidgets 는 안 된다 |
| UI/Common | 시안과 상관없는 부품 | Observers, SystemStatus, 데이터 타입(구조체 · enum) |
| UI/View | 화면 · 대화상자 위젯 | Presenter, UI/Common, Observers, SystemStatus, 데이터 타입. 저장소 · 서비스는 직접 부르지 않는다 |
| UI/Skin/<시안> | 창 배치(.ui), 그 시안만의 화면, qss | Presenter, UI/Common, UI/View. 다른 시안 폴더는 안 된다 |

- 기존 화면 중 저장소 · 서비스를 직접 부르는 것(설정 › 백업, 지식/규칙 화면 등)은 그 화면을 고칠 때 Presenter 로 옮긴다.

### 3.2. Presenter

- `QObject` 이고, 생성자로 Observers · SystemStatus · 저장소/서비스를 주입받는다.
- 뷰가 그릴 값(목록 행, 상태)은 Get 함수로 주고, 바뀌면 시그널로 알린다.
- 사용자 동작은 함수로 받는다. 실패도 시그널로 알리고, 경고 창은 뷰가 띄운다.
- 시안이 달라도 같은 Presenter 를 쓴다. 새 시안은 뷰만 만든다.

```c++
class LearnPresenter : public QObject
{
    Q_OBJECT

public:
    LearnPresenter(Observers* kpObserver, SystemStatus* kpSystem, LearnLibrary* kpLearn, LearnService* kpService, QObject* parent = nullptr);

public:
    void Upload(const QStringList& paths);
    void Remove(const QString& id);

public:
    QVector<LearnRow> GetRows() const;

signals:
    void RowsChanged();
    void SaveFailed();

private:
    LearnLibrary* m_kpLearn = nullptr;
    LearnService* m_kpService = nullptr;
};
```

### 3.3. UI 시안 (Skin)

- 시안 = 창 배치 + 화면 고르기 + qss. `ISkin` 을 구현하고 `SkinRegistry` 에 등록하면 고를 수 있다.
- 배치는 `.ui` 로 그린다. 주입받는 위젯(Observers · SystemStatus 를 받는 위젯)은 `.ui` 에 빈 자리(`QWidget`)를 두고 코드에서 끼운다.
- 시안 qss 는 공통 `Ocean.qss` 뒤에 붙고, 같은 `{{token}}` 을 쓴다.
- 고르기: 설정 › 일반 › UI 시안, 또는 `--skin <id>`(이번 실행만). 바꾸면 창을 다시 만든다. 상태는 SystemStatus 에 있으므로 그대로 이어진다.

```c++
class ISkin
{
public:
    virtual ~ISkin() = default;

public:
    virtual QString GetId() const = 0;              // 설정에 저장하는 값 (예: "default")
    virtual QString GetName() const = 0;            // 설정 화면에 보이는 이름
    virtual QString GetStyleSheetPath() const = 0;  // 시안 qss (예: ":/skin/Compact.qss")
    virtual std::unique_ptr<SkinWindow> CreateMainWindow(const SkinContext& context) const = 0;
};
```