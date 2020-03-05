#include <DApplication>
#include <DApplicationHelper>
#include <DFontSizeManager>
#include <DHeaderView>
#include <DLabel>
#include <DMenu>
#include <DMessageBox>
#include <DSpinner>
#include <QDebug>
#include <QFutureWatcher>
#include <QScrollBar>
#include <QtConcurrent>

#include "common/error_context.h"
#include "main_window.h"
#include "model/system_service_sort_filter_proxy_model.h"
#include "model/system_service_table_model.h"
#include "service/service_manager.h"
#include "service/system_service_entry.h"
#include "service_name_sub_input_dialog.h"
#include "settings.h"
#include "system_service_table_view.h"
#include "toolbar.h"

DWIDGET_USE_NAMESPACE

static const char *kSettingsOption_ServiceTableHeaderState = "service_table_header_state";

// not thread-safe
static bool defer_initialized {false};

SystemServiceTableView::SystemServiceTableView(DWidget *parent)
    : BaseTableView(parent)
{
    installEventFilter(this);

    // >>> table model
    m_ProxyModel = new SystemServiceSortFilterProxyModel(this);
    m_Model = new SystemServiceTableModel(this);
    m_ProxyModel->setSourceModel(m_Model);
    setModel(m_ProxyModel);

    bool settingsLoaded = loadSettings();

    // >>> "not found" display label
    m_noMatchingResultLabel =
        new DLabel(DApplication::translate("Common.Search", "No search results"), this);
    DFontSizeManager::instance()->bind(m_noMatchingResultLabel, DFontSizeManager::T4);
    auto palette = DApplicationHelper::instance()->palette(m_noMatchingResultLabel);
    QColor labelColor = palette.color(DPalette::PlaceholderText);
    palette.setColor(DPalette::Text, labelColor);
    m_noMatchingResultLabel->setPalette(palette);
    m_noMatchingResultLabel->setVisible(false);
    auto *dAppHelper = DApplicationHelper::instance();
    connect(dAppHelper, &DApplicationHelper::themeTypeChanged, this, [ = ]() {
        if (m_noMatchingResultLabel) {
            auto palette = DApplicationHelper::instance()->applicationPalette();
            QColor labelColor = palette.color(DPalette::PlaceholderText);
            palette.setColor(DPalette::Text, labelColor);
            m_noMatchingResultLabel->setPalette(palette);
        }
    });

    QHeaderView *hdr = header();
    hdr->setSectionsMovable(true);
    hdr->setSectionsClickable(true);
    hdr->setSectionResizeMode(DHeaderView::Interactive);
    hdr->setStretchLastSection(true);
    hdr->setSortIndicatorShown(true);
    hdr->setDefaultAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    hdr->setContextMenuPolicy(Qt::CustomContextMenu);

    // table options
    setSortingEnabled(true);
    setSelectionMode(QAbstractItemView::SingleSelection);
    setSelectionBehavior(QAbstractItemView::SelectRows);
    setContextMenuPolicy(Qt::CustomContextMenu);

    // table events
    connect(this, &SystemServiceTableView::customContextMenuRequested, this,
            &SystemServiceTableView::displayTableContextMenu);
    // table header events
    connect(hdr, &QHeaderView::sectionResized, this, [ = ]() { saveSettings(); });
    connect(hdr, &QHeaderView::sectionMoved, this, [ = ]() { saveSettings(); });
    connect(hdr, &QHeaderView::sortIndicatorChanged, this, [ = ]() { saveSettings(); });
    connect(hdr, &QHeaderView::customContextMenuRequested, this,
            &SystemServiceTableView::displayHeaderContextMenu);

    MainWindow *mainWindow = MainWindow::instance();
    connect(mainWindow->toolbar(), &Toolbar::search, this, &SystemServiceTableView::search);

    // >>> table default style
    if (!settingsLoaded) {
        setColumnWidth(SystemServiceTableModel::kSystemServiceNameColumn, 200);
        setColumnHidden(SystemServiceTableModel::kSystemServiceNameColumn, false);
        setColumnWidth(SystemServiceTableModel::kSystemServiceLoadStateColumn, 100);
        setColumnHidden(SystemServiceTableModel::kSystemServiceLoadStateColumn, true);
        setColumnWidth(SystemServiceTableModel::kSystemServiceActiveStateColumn, 100);
        setColumnHidden(SystemServiceTableModel::kSystemServiceActiveStateColumn, false);
        setColumnWidth(SystemServiceTableModel::kSystemServiceSubStateColumn, 100);
        setColumnHidden(SystemServiceTableModel::kSystemServiceSubStateColumn, false);
        setColumnWidth(SystemServiceTableModel::kSystemServiceStateColumn, 100);
        setColumnHidden(SystemServiceTableModel::kSystemServiceStateColumn, false);
        setColumnWidth(SystemServiceTableModel::kSystemServiceStartupModeColumn, 80);
        setColumnHidden(SystemServiceTableModel::kSystemServiceStartupModeColumn, true);
        setColumnWidth(SystemServiceTableModel::kSystemServiceDescriptionColumn, 320);
        setColumnHidden(SystemServiceTableModel::kSystemServiceDescriptionColumn, false);
        setColumnWidth(SystemServiceTableModel::kSystemServicePIDColumn, 100);
        setColumnHidden(SystemServiceTableModel::kSystemServicePIDColumn, true);
        sortByColumn(SystemServiceTableModel::kSystemServiceNameColumn, Qt::AscendingOrder);
    }

    // >>> service tableview context menu
    m_contextMenu = new DMenu(this);
    // start service
    QAction *startServiceAction =
        m_contextMenu->addAction(DApplication::translate("Service.Table.Context.Menu", "Start"));
    startServiceAction->setShortcut(QKeySequence(Qt::ALT + Qt::Key_S));
    connect(startServiceAction, &QAction::triggered, this, &SystemServiceTableView::startService);
    // stop service
    QAction *stopServiceAction =
        m_contextMenu->addAction(DApplication::translate("Service.Table.Context.Menu", "Stop"));
    stopServiceAction->setShortcut(QKeySequence(Qt::ALT + Qt::Key_T));
    connect(stopServiceAction, &QAction::triggered, this, &SystemServiceTableView::stopService);
    // restart service
    QAction *restartServiceAction =
        m_contextMenu->addAction(DApplication::translate("Service.Table.Context.Menu", "Restart"));
    restartServiceAction->setShortcut(QKeySequence(Qt::ALT + Qt::Key_R));
    connect(restartServiceAction, &QAction::triggered, this,
            &SystemServiceTableView::restartService);
    // service startup mode
    auto *setServiceStartupModeMenu =
        m_contextMenu->addMenu(DApplication::translate("Service.Table.Context.Menu", "Startup type"));
    auto *setAutoStartAction = setServiceStartupModeMenu->addAction(DApplication::translate("Service.Table.Context.Menu", "Auto"));
    auto *setManualStartAction = setServiceStartupModeMenu->addAction(DApplication::translate("Service.Table.Context.Menu", "Manual"));
    connect(setAutoStartAction, &QAction::triggered, this, [ = ]() { setServiceStartupMode(true); });
    connect(setManualStartAction, &QAction::triggered, this, [ = ]() { setServiceStartupMode(false); });
    // refresh context menu item
    QAction *refreshAction =
        m_contextMenu->addAction(DApplication::translate("Service.Table.Context.Menu", "Refresh"));
    refreshAction->setShortcut(QKeySequence(QKeySequence::Refresh));
    connect(refreshAction, &QAction::triggered, this, &SystemServiceTableView::refresh);

    connect(m_contextMenu, &QMenu::aboutToShow, this, [ = ]() {
        if (selectionModel()->selectedRows().size() > 0) {
            // proxy index
            auto index = selectionModel()->selectedRows()[0];
            if (index.isValid()) {
                auto state = m_Model->getUnitFileState(m_ProxyModel->mapToSource(index));
                auto uname = m_Model->getUnitFileName(m_ProxyModel->mapToSource(index));
                if (isUnitNoOp(state) || uname.endsWith("@")) {
                    setServiceStartupModeMenu->setEnabled(false);
                } else {
                    setServiceStartupModeMenu->setEnabled(true);
                }

                auto sname = m_Model->getUnitFileName(m_ProxyModel->mapToSource(index));
                if (isServiceAutoStartup(sname, state)) {
                    setAutoStartAction->setEnabled(false);
                    setManualStartAction->setEnabled(true);
                } else {
                    setAutoStartAction->setEnabled(true);
                    setManualStartAction->setEnabled(false);
                }
            }
        }
    });

    // >>> header context menu
    m_headerContextMenu = new DMenu(this);
    // load state column
    QAction *loadStateHeaderAction = m_headerContextMenu->addAction(
                                         DApplication::translate("Service.Table.Header", kSystemServiceLoadState));
    loadStateHeaderAction->setCheckable(true);
    connect(loadStateHeaderAction, &QAction::triggered, this, [this](bool b) {
        header()->setSectionHidden(SystemServiceTableModel::kSystemServiceLoadStateColumn, !b);
        saveSettings();
    });
    // active state column
    QAction *activeStateHeaderAction = m_headerContextMenu->addAction(
                                           DApplication::translate("Service.Table.Header", kSystemServiceActiveState));
    activeStateHeaderAction->setCheckable(true);
    connect(activeStateHeaderAction, &QAction::triggered, this, [this](bool b) {
        header()->setSectionHidden(SystemServiceTableModel::kSystemServiceActiveStateColumn, !b);
        saveSettings();
    });
    // sub state column
    QAction *subStateHeaderAction = m_headerContextMenu->addAction(
                                        DApplication::translate("Service.Table.Header", kSystemServiceSubState));
    subStateHeaderAction->setCheckable(true);
    connect(subStateHeaderAction, &QAction::triggered, this, [this](bool b) {
        header()->setSectionHidden(SystemServiceTableModel::kSystemServiceSubStateColumn, !b);
        saveSettings();
    });
    // state column
    QAction *stateHeaderAction = m_headerContextMenu->addAction(
                                     DApplication::translate("Service.Table.Header", kSystemServiceState));
    stateHeaderAction->setCheckable(true);
    connect(stateHeaderAction, &QAction::triggered, this, [this](bool b) {
        header()->setSectionHidden(SystemServiceTableModel::kSystemServiceStateColumn, !b);
        saveSettings();
    });
    // description column
    QAction *descriptionHeaderAction = m_headerContextMenu->addAction(
                                           DApplication::translate("Service.Table.Header", kSystemServiceDescription));
    descriptionHeaderAction->setCheckable(true);
    connect(descriptionHeaderAction, &QAction::triggered, this, [this](bool b) {
        header()->setSectionHidden(SystemServiceTableModel::kSystemServiceDescriptionColumn, !b);
        saveSettings();
    });
    // pid column
    QAction *pidHeaderAction = m_headerContextMenu->addAction(
                                   DApplication::translate("Service.Table.Header", kSystemServicePID));
    pidHeaderAction->setCheckable(true);
    connect(pidHeaderAction, &QAction::triggered, this, [this](bool b) {
        header()->setSectionHidden(SystemServiceTableModel::kSystemServicePIDColumn, !b);
        saveSettings();
    });
    // startup mode column
    auto *startupModeHeaderAction = m_headerContextMenu->addAction(
                                        DApplication::translate("Service.Table.Header", kSystemServiceStartupMode));
    startupModeHeaderAction->setCheckable(true);
    connect(startupModeHeaderAction, &QAction::triggered, this, [this](bool b) {
        header()->setSectionHidden(SystemServiceTableModel::kSystemServiceStartupModeColumn, !b);
        saveSettings();
    });

    if (!settingsLoaded) {
        loadStateHeaderAction->setChecked(false);
        activeStateHeaderAction->setChecked(true);
        subStateHeaderAction->setChecked(true);
        stateHeaderAction->setChecked(true);
        descriptionHeaderAction->setChecked(true);
        pidHeaderAction->setChecked(false);
    }
    connect(m_headerContextMenu, &QMenu::aboutToShow, this, [ = ]() {
        bool b;
        b = header()->isSectionHidden(SystemServiceTableModel::kSystemServiceLoadStateColumn);
        loadStateHeaderAction->setChecked(!b);
        b = header()->isSectionHidden(SystemServiceTableModel::kSystemServiceActiveStateColumn);
        activeStateHeaderAction->setChecked(!b);
        b = header()->isSectionHidden(SystemServiceTableModel::kSystemServiceSubStateColumn);
        subStateHeaderAction->setChecked(!b);
        b = header()->isSectionHidden(SystemServiceTableModel::kSystemServiceStateColumn);
        stateHeaderAction->setChecked(!b);
        b = header()->isSectionHidden(SystemServiceTableModel::kSystemServiceStartupModeColumn);
        startupModeHeaderAction->setChecked(!b);
        b = header()->isSectionHidden(SystemServiceTableModel::kSystemServiceDescriptionColumn);
        descriptionHeaderAction->setChecked(!b);
        b = header()->isSectionHidden(SystemServiceTableModel::kSystemServicePIDColumn);
        pidHeaderAction->setChecked(!b);
    });

    m_refreshKP = new QShortcut(QKeySequence(QKeySequence::Refresh), this);
    connect(m_refreshKP, &QShortcut::activated, this, &SystemServiceTableView::refresh);
    m_startKP = new QShortcut(QKeySequence(Qt::ALT + Qt::Key_S), this);
    connect(m_startKP, &QShortcut::activated, this, &SystemServiceTableView::startService);
    m_stopKP = new QShortcut(QKeySequence(Qt::ALT + Qt::Key_T), this);
    connect(m_stopKP, &QShortcut::activated, this, &SystemServiceTableView::stopService);
    m_restartKP = new QShortcut(QKeySequence(Qt::ALT + Qt::Key_R), this);
    connect(m_restartKP, &QShortcut::activated, this, &SystemServiceTableView::restartService);

    m_spinner = new DSpinner(this);
    auto pa = DApplicationHelper::instance()->applicationPalette();
    QBrush hlBrush = pa.color(DPalette::Active, DPalette::Highlight);
    pa.setColor(DPalette::Highlight, hlBrush.color());
    m_spinner->setPalette(pa);
    m_spinner->move(rect().center() - m_spinner->rect().center());

    // initialize service list
    MainWindow *mwnd = MainWindow::instance();
    if (mwnd) {
        auto *tbar = mwnd->toolbar();
        connect(tbar, &Toolbar::serviceTabButtonClicked, this, [ = ]() {
            if (!defer_initialized) {
                asyncGetServiceEntryList();
                defer_initialized = true;
            }
        });
    }

    auto *mgr = ServiceManager::instance();
    Q_ASSERT(mgr != nullptr);
    connect(mgr, &ServiceManager::errorOccurred, this, [ = ](const ErrorContext & ec) {
        if (ec) {
            DMessageBox::critical(this, ec.getErrorName(), ec.getErrorMessage());
        }
    });
}

SystemServiceTableView::~SystemServiceTableView()
{
    saveSettings();
}

void SystemServiceTableView::saveSettings()
{
    Settings *s = Settings::instance();
    if (s) {
        QByteArray buf = header()->saveState();
        s->setOption(kSettingsOption_ServiceTableHeaderState, buf.toBase64());
        s->flush();
    }
}

bool SystemServiceTableView::loadSettings()
{
    Settings *s = Settings::instance();
    if (s) {
        QVariant opt = s->getOption(kSettingsOption_ServiceTableHeaderState);
        if (opt.isValid()) {
            QByteArray buf = QByteArray::fromBase64(opt.toByteArray());
            header()->restoreState(buf);
            return true;
        }
    }
    return false;
}

void SystemServiceTableView::displayHeaderContextMenu(const QPoint &p)
{
    m_headerContextMenu->popup(mapToGlobal(p));
}

void SystemServiceTableView::displayTableContextMenu(const QPoint &p)
{
    if (selectedIndexes().size() == 0)
        return;

    QPoint point = mapToGlobal(p);
    // when popup context menu for items, take table header height into consideration
    point.setY(point.y() + header()->sizeHint().height());
    m_contextMenu->popup(point);
}

int SystemServiceTableView::getSelectedServiceEntry(SystemServiceEntry &entry) const
{
    QModelIndexList list = selectedIndexes();
    if (list.size() > 0) {
        QModelIndex index = m_ProxyModel->mapToSource(list.at(0));
        SystemServiceEntry e = getSourceModel()->getSystemServiceEntry(index);
        if (!e.getId().isEmpty()) {
            entry = e;
            return list.at(0).row();
        }
    }

    return -1;
}

void SystemServiceTableView::startService()
{
    ErrorContext ec;
    SystemServiceEntry entry;
    int row = getSelectedServiceEntry(entry);
    if (row < 0)
        return;

    bool newRow = false;

    // dialog
    if (entry.getId().endsWith('@')) {
        ServiceNameSubInputDialog dialog(this);
        dialog.setTitle(
            DApplication::translate("Service.Instance.Name.Dialog", "Service instance name"));
        dialog.setMessage(entry.getDescription());
        dialog.exec();
        if (dialog.result() == QMessageBox::Ok) {
            QString name = entry.getId().append(dialog.getServiceSubName());

            // already in the list
            QModelIndex idx = getSourceModel()->checkServiceEntryExists(name);
            if (idx.isValid()) {
                QItemSelectionModel *selection = selectionModel();
                selection->select(m_ProxyModel->mapFromSource(idx),
                                  QItemSelectionModel::SelectCurrent | QItemSelectionModel::Rows |
                                  QItemSelectionModel::Clear);
                setSelectionModel(selection);
                row = getSelectedServiceEntry(entry);
            } else {  // otherwise, add a new entry
                entry = {};
                entry.setId(name);
                newRow = true;
            }
        } else {  // cancel clicked
            return;
        }
    }

    ServiceManager *mgr = ServiceManager::instance();

    auto result = mgr->startService(entry);
    ec = result.first;
    if (ec) {
        handleTaskError(ec);
    } else {
        if (newRow) {
            QModelIndex idx = getSourceModel()->insertServiceEntry(entry);
            QItemSelectionModel *selection = selectionModel();
            selection->select(m_ProxyModel->mapFromSource(idx), QItemSelectionModel::SelectCurrent |
                              QItemSelectionModel::Rows |
                              QItemSelectionModel::Clear);
            setSelectionModel(selection);
        } else {
            getSourceModel()->updateServiceEntry(row);
        }
    }
}

void SystemServiceTableView::stopService()
{
    ErrorContext ec;
    SystemServiceEntry entry;
    bool deleteItem = false;
    SystemServiceEntry opt {};
    QModelIndex rowIndex;

    int row = getSelectedServiceEntry(entry);
    if (row < 0)
        return;

    // service id syntax: xxx@
    if (entry.getId().endsWith('@')) {
        ServiceNameSubInputDialog dialog(this);
        dialog.setTitle(
            DApplication::translate("Service.Instance.Name.Dialog", "Service instance name"));
        dialog.setMessage(entry.getDescription());
        dialog.exec();
        if (dialog.result() == QMessageBox::Ok) {
            QString name = entry.getId();

            opt.setId(name.append(dialog.getServiceSubName()));

            rowIndex = getSourceModel()->checkServiceEntryExists(name);
            if (rowIndex.isValid()) {
                deleteItem = true;
            }
        } else {
            return;
        }
        // serivce id syntax: xxx@yyy
    } else if (entry.getId().contains(QRegularExpression(
                                          "^[^@\\.]+@[^@\\.]+$", QRegularExpression::CaseInsensitiveOption))) {
        deleteItem = true;
        opt.setId(entry.getId());
        rowIndex = getSourceModel()->checkServiceEntryExists(entry.getId());
    } else {
        opt.setId(entry.getId());
    }

    ServiceManager *mgr = ServiceManager::instance();

    auto result = mgr->stopService(opt);
    ec = result.first;
    if (ec) {
        handleTaskError(ec);
    } else {
        if (deleteItem) {
            getSourceModel()->removeServiceEntry(rowIndex);
        } else {
            entry.setId(opt.getId());
            entry.setLoadState(opt.getLoadState());
            entry.setActiveState(opt.getActiveState());
            entry.setSubState(opt.getSubState());
            entry.setState(opt.getState());
            entry.setUnitObjectPath(opt.getUnitObjectPath());
            entry.setDescription(opt.getDescription());
            entry.setMainPID(opt.getMainPID());
            entry.setCanReload(opt.getCanReload());
            entry.setCanStop(opt.getCanStop());
            entry.setCanStart(opt.getCanStart());
            getSourceModel()->updateServiceEntry(row);
        }
    }
}

void SystemServiceTableView::restartService()
{
    ErrorContext ec;
    SystemServiceEntry entry;
    int row = getSelectedServiceEntry(entry);
    if (row < 0)
        return;

    ServiceManager *mgr = ServiceManager::instance();

    auto result = mgr->restartService(entry);
    ec = result.first;
    if (ec) {
        handleTaskError(ec);
    } else {
        getSourceModel()->updateServiceEntry(row);
    }
}

void SystemServiceTableView::setServiceStartupMode(bool autoStart)
{
    ErrorContext ec;
    SystemServiceEntry entry;
    int row = getSelectedServiceEntry(entry);
    if (row < 0)
        return;

    auto *mgr = ServiceManager::instance();
    Q_ASSERT(mgr != nullptr);

    mgr->setServiceStartupMode(entry.getId(), autoStart);
}

void SystemServiceTableView::handleTaskError(const ErrorContext &ec) const
{
    MainWindow *mainWindow = MainWindow::instance();
    DMessageBox::critical(mainWindow, ec.getErrorName(), ec.getErrorMessage());
}

void SystemServiceTableView::adjustInfoLabelVisibility()
{
    setUpdatesEnabled(false);
    m_noMatchingResultLabel->setVisible(m_ProxyModel->rowCount() == 0 && m_spinner &&
                                        !m_spinner->isPlaying());
    if (m_noMatchingResultLabel->isVisible())
        m_noMatchingResultLabel->move(rect().center() - m_noMatchingResultLabel->rect().center());
    setUpdatesEnabled(true);
}

void SystemServiceTableView::search(const QString &pattern)
{
    m_ProxyModel->setFilterRegExp(QRegExp(pattern, Qt::CaseInsensitive));

    adjustInfoLabelVisibility();
}

void SystemServiceTableView::resizeEvent(QResizeEvent *event)
{
    if (m_spinner) {
        m_spinner->move(rect().center() - m_spinner->rect().center());
    }
    adjustInfoLabelVisibility();

    DTreeView::resizeEvent(event);
}

int SystemServiceTableView::sizeHintForColumn(int column) const
{
    QStyleOptionHeader option;
    option.initFrom(this);
    DStyle *style = dynamic_cast<DStyle *>(DApplication::style());
    int margin = style->pixelMetric(DStyle::PM_ContentsMargins, &option);

    return std::max(header()->sizeHintForColumn(column) + margin * 2,
                    DTreeView::sizeHintForColumn(column) + margin * 2);
}

void SystemServiceTableView::refresh()
{
    if (m_loading)
        return;

    m_Model->removeAll();
    asyncGetServiceEntryList();
}

SystemServiceTableModel *SystemServiceTableView::getSourceModel() const
{
    return m_Model;
}

bool SystemServiceTableView::eventFilter(QObject *obj, QEvent *event)
{
    if (obj == this) {
        if (event->type() == QEvent::KeyPress) {
            auto *kev = dynamic_cast<QKeyEvent *>(event);
            if (kev->modifiers() & Qt::CTRL && kev->key() == Qt::Key_M) {
                if (this->hasFocus()) {
                    if (selectedIndexes().size() > 0) {
                        auto index = selectedIndexes()[0];
                        auto rect = visualRect(index);
                        displayTableContextMenu({rect.x() + rect.width() / 2, rect.y() + rect.height() / 2});
                        return true;
                    }
                } else if (header()->hasFocus()) {
                    displayHeaderContextMenu({header()->sectionSize(header()->logicalIndexAt(0)) / 2, header()->height() / 2});
                    return true;
                }
            }
        }
    }
    return BaseTableView::eventFilter(obj, event);
}

// gui-thread
void SystemServiceTableView::asyncGetServiceEntryList()
{
    m_loading = true;
    auto *mwnd = MainWindow::instance();
    Q_EMIT mwnd->loadingStatusChanged(m_loading);

    m_noMatchingResultLabel->hide();
    m_spinner->start();
    m_spinner->show();

    auto *watcher = new QFutureWatcher<QPair<ErrorContext, QList<SystemServiceEntry>>>;
    QFuture<QPair<ErrorContext, QList<SystemServiceEntry>>> future;
    QObject::connect(watcher, &QFutureWatcher<void>::finished, [ = ]() {
        auto fe = watcher->future();
        resetModel(fe.result().first, fe.result().second);
        watcher->deleteLater();
    });
    future = QtConcurrent::run(this, &SystemServiceTableView::processAsyncGetServiceListTask);
    watcher->setFuture(future);
}

// thread-pool job!!
QPair<ErrorContext, QList<SystemServiceEntry>>
                                            SystemServiceTableView::processAsyncGetServiceListTask()
{
    ServiceManager *mgr = ServiceManager::instance();
    Q_ASSERT(mgr != nullptr);
    return mgr->getServiceEntryList();
}

// gui-thread
void SystemServiceTableView::resetModel(const ErrorContext &ec,
                                        const QList<SystemServiceEntry> &list)
{
    if (ec) {
        handleTaskError(ec);
        return;
    }
    m_Model->setServiceEntryList(list);

    m_spinner->hide();
    m_spinner->stop();

    adjustInfoLabelVisibility();

    m_loading = false;
    auto *mwnd = MainWindow::instance();
    Q_EMIT mwnd->loadingStatusChanged(m_loading);
}
