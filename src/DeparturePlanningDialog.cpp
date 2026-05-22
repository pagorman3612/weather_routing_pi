/*
 * DeparturePlanningDialog.cpp — WR.3 Departure Planning Dialog
 *
 * Contract: docs/DEPARTURE_SWEEP_CONTRACT_FINAL-1.md §5.
 */
#include <wx/wx.h>
#include <wx/fileconf.h>
#include <wx/filedlg.h>
#include <wx/datectrl.h>
#include <wx/timectrl.h>
#include <wx/dateevt.h>

#include "DeparturePlanningDialog.h"
#include "WeatherRouting.h"
#include "RouteMapOverlay.h"
#include "StatisticsDialog.h"
#include "PlotDialog.h"

// ---------------------------------------------------------------------------
// Step-size table (seconds)
// ---------------------------------------------------------------------------

static const int kStepSecs[] = { 3600, 7200, 10800, 21600, 43200, 86400, 0 };
static const wxString kStepLabels[] = {
    "1 h", "2 h", "3 h (default)", "6 h", "12 h", "24 h", "GRIB spacing (auto)"
};

// ---------------------------------------------------------------------------
// Ctor / dtor
// ---------------------------------------------------------------------------

DeparturePlanningDialog::DeparturePlanningDialog(wxWindow* parent, WeatherRouting& wr)
    : wxDialog(parent, wxID_ANY, _("Departure Planning"),
               wxDefaultPosition, wxSize(900, 700),
               wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER)
    , m_wr(wr)
    , m_pollTimer(this, wxID_ANY)
{
    BuildUI();
    LoadConfig();
    UpdateBaseRoute();
    Bind(wxEVT_TIMER, &DeparturePlanningDialog::OnPollTimer, this, m_pollTimer.GetId());
}

DeparturePlanningDialog::~DeparturePlanningDialog() {
    SaveConfig();
    m_pollTimer.Stop();
    m_controller.FreeOverlays();
}

// ---------------------------------------------------------------------------
// UI construction
// ---------------------------------------------------------------------------

void DeparturePlanningDialog::BuildUI() {
    wxBoxSizer* top = new wxBoxSizer(wxVERTICAL);
    SetSizer(top);

    // --- Base route label ---
    wxStaticBoxSizer* sbRoute = new wxStaticBoxSizer(
        new wxStaticBox(this, wxID_ANY, _("Base Route")), wxHORIZONTAL);
    m_stBaseRoute = new wxStaticText(this, wxID_ANY, _("(none)"));
    sbRoute->Add(m_stBaseRoute, 1, wxALL | wxALIGN_CENTER_VERTICAL, 4);
    top->Add(sbRoute, 0, wxEXPAND | wxALL, 4);

    // --- Time display ---
    wxStaticBoxSizer* sbTz = new wxStaticBoxSizer(
        new wxStaticBox(this, wxID_ANY, _("Time Display")), wxHORIZONTAL);
    m_rbUtc   = new wxRadioButton(this, wxID_ANY, _("UTC"),        wxDefaultPosition, wxDefaultSize, wxRB_GROUP);
    m_rbLocal = new wxRadioButton(this, wxID_ANY, _("Local time"));
    sbTz->Add(m_rbUtc,   0, wxALL | wxALIGN_CENTER_VERTICAL, 4);
    sbTz->Add(m_rbLocal, 0, wxALL | wxALIGN_CENTER_VERTICAL, 4);
    sbTz->Add(new wxStaticText(this, wxID_ANY, _("UTC offset:")), 0, wxALL | wxALIGN_CENTER_VERTICAL, 4);
    m_stTzSign = new wxStaticText(this, wxID_ANY, "+");
    m_spTzHour = new wxSpinCtrl(this, wxID_ANY, "0", wxDefaultPosition, wxSize(50, -1),
                                wxSP_ARROW_KEYS, -12, 14, 0);
    m_spTzMin  = new wxSpinCtrl(this, wxID_ANY, "00", wxDefaultPosition, wxSize(50, -1),
                                wxSP_ARROW_KEYS, 0, 30, 0); // 0 or 30
    sbTz->Add(m_stTzSign, 0, wxALL | wxALIGN_CENTER_VERTICAL, 2);
    sbTz->Add(m_spTzHour, 0, wxALL | wxALIGN_CENTER_VERTICAL, 2);
    sbTz->Add(new wxStaticText(this, wxID_ANY, ":"), 0, wxALIGN_CENTER_VERTICAL);
    sbTz->Add(m_spTzMin,  0, wxALL | wxALIGN_CENTER_VERTICAL, 2);
    m_rbUtc->SetValue(true);
    top->Add(sbTz, 0, wxEXPAND | wxALL, 4);

    m_rbUtc->Bind(wxEVT_RADIOBUTTON,  &DeparturePlanningDialog::OnUtcLocal, this);
    m_rbLocal->Bind(wxEVT_RADIOBUTTON, &DeparturePlanningDialog::OnUtcLocal, this);

    // --- Departure window ---
    wxStaticBoxSizer* sbWin = new wxStaticBoxSizer(
        new wxStaticBox(this, wxID_ANY, _("Departure Window")), wxHORIZONTAL);
    sbWin->Add(new wxStaticText(this, wxID_ANY, _("Start:")), 0, wxALIGN_CENTER_VERTICAL | wxALL, 4);
    m_dpStart = new wxDatePickerCtrl(this, wxID_ANY, wxDefaultDateTime, wxDefaultPosition, wxSize(110, -1));
    m_tpStart = new wxTimePickerCtrl(this, wxID_ANY, wxDefaultDateTime, wxDefaultPosition, wxSize(80, -1));
    sbWin->Add(m_dpStart, 0, wxALL | wxALIGN_CENTER_VERTICAL, 2);
    sbWin->Add(m_tpStart, 0, wxALL | wxALIGN_CENTER_VERTICAL, 2);
    sbWin->AddSpacer(12);
    sbWin->Add(new wxStaticText(this, wxID_ANY, _("End:")), 0, wxALIGN_CENTER_VERTICAL | wxALL, 4);
    m_dpEnd = new wxDatePickerCtrl(this, wxID_ANY, wxDefaultDateTime, wxDefaultPosition, wxSize(110, -1));
    m_tpEnd = new wxTimePickerCtrl(this, wxID_ANY, wxDefaultDateTime, wxDefaultPosition, wxSize(80, -1));
    sbWin->Add(m_dpEnd, 0, wxALL | wxALIGN_CENTER_VERTICAL, 2);
    sbWin->Add(m_tpEnd, 0, wxALL | wxALIGN_CENTER_VERTICAL, 2);
    sbWin->AddSpacer(12);
    sbWin->Add(new wxStaticText(this, wxID_ANY, _("Step:")), 0, wxALIGN_CENTER_VERTICAL | wxALL, 4);
    m_chStep = new wxChoice(this, wxID_ANY);
    for (const wxString& lbl : kStepLabels) m_chStep->Append(lbl);
    m_chStep->SetSelection(2); // 3 h default
    sbWin->Add(m_chStep, 0, wxALL | wxALIGN_CENTER_VERTICAL, 2);
    top->Add(sbWin, 0, wxEXPAND | wxALL, 4);

    // Default times: now rounded to hour / now + 5 days
    wxDateTime now = wxDateTime::Now().ToUTC();
    now.SetSecond(0); now.SetMinute(0);
    m_dpStart->SetValue(now); m_tpStart->SetValue(now);
    wxDateTime end5 = now + wxTimeSpan::Days(5);
    m_dpEnd->SetValue(end5); m_tpEnd->SetValue(end5);

    // --- Ranking ---
    wxStaticBoxSizer* sbRank = new wxStaticBoxSizer(
        new wxStaticBox(this, wxID_ANY, _("Ranking")), wxHORIZONTAL);
    sbRank->Add(new wxStaticText(this, wxID_ANY, _("Mode:")), 0, wxALIGN_CENTER_VERTICAL | wxALL, 4);
    m_chRankMode = new wxChoice(this, wxID_ANY);
    m_chRankMode->Append(_("Balanced"));
    m_chRankMode->Append(_("Fastest"));
    m_chRankMode->Append(_("Smoothest"));
    m_chRankMode->SetSelection(0);
    sbRank->Add(m_chRankMode, 0, wxALL | wxALIGN_CENTER_VERTICAL, 2);
    m_pnlWeight = new wxPanel(this, wxID_ANY);
    wxBoxSizer* wsz = new wxBoxSizer(wxHORIZONTAL);
    wsz->Add(new wxStaticText(m_pnlWeight, wxID_ANY, _("Weight:")), 0, wxALIGN_CENTER_VERTICAL | wxALL, 4);
    m_slWeight = new wxSlider(m_pnlWeight, wxID_ANY, 5, 0, 10, wxDefaultPosition, wxSize(120, -1));
    m_stWeight = new wxStaticText(m_pnlWeight, wxID_ANY, "0.5");
    wsz->Add(m_slWeight, 0, wxALL | wxALIGN_CENTER_VERTICAL, 2);
    wsz->Add(m_stWeight, 0, wxALL | wxALIGN_CENTER_VERTICAL, 4);
    m_pnlWeight->SetSizer(wsz);
    sbRank->Add(m_pnlWeight, 0, wxALL | wxALIGN_CENTER_VERTICAL, 2);
    top->Add(sbRank, 0, wxEXPAND | wxALL, 4);

    m_chRankMode->Bind(wxEVT_CHOICE, &DeparturePlanningDialog::OnRankModeChange, this);
    m_slWeight->Bind(wxEVT_SLIDER,   &DeparturePlanningDialog::OnRankWeightChange, this);

    // --- Comfort profile ---
    wxStaticBoxSizer* sbProf = new wxStaticBoxSizer(
        new wxStaticBox(this, wxID_ANY, _("Comfort Profile (optional)")), wxHORIZONTAL);
    m_tcProfilePath   = new wxTextCtrl(this, wxID_ANY, wxEmptyString,
                                       wxDefaultPosition, wxSize(300, -1),
                                       wxTE_READONLY);
    wxButton* btnBrowse = new wxButton(this, wxID_ANY, _("Browse..."));
    m_stProfileStatus = new wxStaticText(this, wxID_ANY, wxEmptyString);
    sbProf->Add(m_tcProfilePath,   1, wxALL | wxALIGN_CENTER_VERTICAL, 4);
    sbProf->Add(btnBrowse,         0, wxALL | wxALIGN_CENTER_VERTICAL, 2);
    sbProf->Add(m_stProfileStatus, 0, wxALL | wxALIGN_CENTER_VERTICAL, 4);
    top->Add(sbProf, 0, wxEXPAND | wxALL, 4);
    btnBrowse->Bind(wxEVT_BUTTON, &DeparturePlanningDialog::OnBrowseProfile, this);

    // --- Arrival window ---
    wxStaticBoxSizer* sbArr = new wxStaticBoxSizer(
        new wxStaticBox(this, wxID_ANY, _("Arrival Window")), wxVERTICAL);
    m_cbArrival = new wxCheckBox(this, wxID_ANY, _("Constrain arrival time"));
    sbArr->Add(m_cbArrival, 0, wxALL, 4);
    m_pnlArrivalControls = new wxPanel(this, wxID_ANY);
    wxBoxSizer* asz = new wxBoxSizer(wxVERTICAL);
    // Mode row
    wxBoxSizer* modeRow = new wxBoxSizer(wxHORIZONTAL);
    m_rbFixed  = new wxRadioButton(m_pnlArrivalControls, wxID_ANY, _("Fixed time window"),
                                   wxDefaultPosition, wxDefaultSize, wxRB_GROUP);
    m_rbSunset = new wxRadioButton(m_pnlArrivalControls, wxID_ANY, _("Before sunset at destination"));
    modeRow->Add(m_rbFixed,  0, wxALL | wxALIGN_CENTER_VERTICAL, 4);
    modeRow->Add(m_rbSunset, 0, wxALL | wxALIGN_CENTER_VERTICAL, 4);
    asz->Add(modeRow, 0, wxEXPAND);
    m_rbSunset->SetValue(true);
    // Fixed panel
    m_pnlFixed = new wxPanel(m_pnlArrivalControls, wxID_ANY);
    wxBoxSizer* fsz = new wxBoxSizer(wxHORIZONTAL);
    fsz->Add(new wxStaticText(m_pnlFixed, wxID_ANY, _("Arrive between:")),
             0, wxALIGN_CENTER_VERTICAL | wxALL, 4);
    m_tpEarliest = new wxTimePickerCtrl(m_pnlFixed, wxID_ANY, wxDefaultDateTime,
                                        wxDefaultPosition, wxSize(80, -1));
    fsz->Add(m_tpEarliest, 0, wxALL | wxALIGN_CENTER_VERTICAL, 2);
    fsz->Add(new wxStaticText(m_pnlFixed, wxID_ANY, _("and")),
             0, wxALIGN_CENTER_VERTICAL | wxALL, 4);
    m_tpLatest = new wxTimePickerCtrl(m_pnlFixed, wxID_ANY, wxDefaultDateTime,
                                      wxDefaultPosition, wxSize(80, -1));
    fsz->Add(m_tpLatest, 0, wxALL | wxALIGN_CENTER_VERTICAL, 2);
    m_pnlFixed->SetSizer(fsz);
    wxDateTime t6(6, 0, 0), t20(20, 0, 0);
    m_tpEarliest->SetValue(t6); m_tpLatest->SetValue(t20);
    asz->Add(m_pnlFixed, 0, wxEXPAND | wxLEFT, 16);
    m_pnlFixed->Hide();
    // Sunset panel
    m_pnlSunset = new wxPanel(m_pnlArrivalControls, wxID_ANY);
    wxBoxSizer* ssz = new wxBoxSizer(wxHORIZONTAL);
    ssz->Add(new wxStaticText(m_pnlSunset, wxID_ANY, _("Hours before sunset:")),
             0, wxALIGN_CENTER_VERTICAL | wxALL, 4);
    m_spMargin = new wxSpinCtrlDouble(m_pnlSunset, wxID_ANY, "0.0",
                                      wxDefaultPosition, wxSize(70, -1),
                                      wxSP_ARROW_KEYS, 0.0, 6.0, 0.0, 0.5);
    ssz->Add(m_spMargin, 0, wxALL | wxALIGN_CENTER_VERTICAL, 2);
    m_pnlSunset->SetSizer(ssz);
    asz->Add(m_pnlSunset, 0, wxEXPAND | wxLEFT, 16);
    m_pnlArrivalControls->SetSizer(asz);
    sbArr->Add(m_pnlArrivalControls, 0, wxEXPAND | wxALL, 4);
    m_pnlArrivalControls->Disable();
    top->Add(sbArr, 0, wxEXPAND | wxALL, 4);

    m_cbArrival->Bind(wxEVT_CHECKBOX,    &DeparturePlanningDialog::OnArrivalFilterCheck, this);
    m_rbFixed->Bind(wxEVT_RADIOBUTTON,   &DeparturePlanningDialog::OnArrivalModeChange, this);
    m_rbSunset->Bind(wxEVT_RADIOBUTTON,  &DeparturePlanningDialog::OnArrivalModeChange, this);

    // --- Results list ---
    m_lcResults = new wxListCtrl(this, wxID_ANY, wxDefaultPosition, wxSize(-1, 200),
                                  wxLC_REPORT | wxLC_SINGLE_SEL | wxBORDER_SUNKEN);
    static const wxString colHdrs[COL_COUNT] = {
        _("Rank"), _("Departure (UTC)"), _("ETA (UTC)"), _("Duration"),
        _("Avg TWS (kn)"), _("Max TWS (kn)"), _("Max Swell (m)"),
        _("Comfort"), _("Arrival"), _("Status")
    };
    static const int colWidths[COL_COUNT] = { 45, 145, 145, 80, 90, 90, 90, 70, 85, 120 };
    for (int i = 0; i < COL_COUNT; ++i)
        m_lcResults->InsertColumn(i, colHdrs[i], wxLIST_FORMAT_LEFT, colWidths[i]);
    top->Add(m_lcResults, 1, wxEXPAND | wxALL, 4);

    // --- Progress + status ---
    m_gauge    = new wxGauge(this, wxID_ANY, 100, wxDefaultPosition, wxSize(-1, 14));
    m_stStatus = new wxStaticText(this, wxID_ANY, wxEmptyString);
    top->Add(m_gauge,    0, wxEXPAND | wxALL, 4);
    top->Add(m_stStatus, 0, wxALL, 4);
    m_gauge->Hide();

    // --- Buttons ---
    wxBoxSizer* btnRow = new wxBoxSizer(wxHORIZONTAL);
    m_btnRun    = new wxButton(this, wxID_ANY, _("Run"));
    m_btnApply  = new wxButton(this, wxID_ANY, _("Apply"));
    m_btnInspect = new wxButton(this, wxID_ANY, _("Inspect"));
    wxButton* btnClose = new wxButton(this, wxID_ANY, _("Close"));
    btnRow->Add(m_btnRun,     0, wxALL, 4);
    btnRow->AddStretchSpacer();
    btnRow->Add(m_btnApply,   0, wxALL, 4);
    btnRow->Add(m_btnInspect, 0, wxALL, 4);
    btnRow->Add(btnClose,     0, wxALL, 4);
    top->Add(btnRow, 0, wxEXPAND | wxALL, 4);

    m_btnApply->Disable();
    m_btnInspect->Disable();

    m_btnRun->Bind(wxEVT_BUTTON,    &DeparturePlanningDialog::OnRun, this);
    m_btnApply->Bind(wxEVT_BUTTON,  &DeparturePlanningDialog::OnApply, this);
    m_btnInspect->Bind(wxEVT_BUTTON, &DeparturePlanningDialog::OnInspect, this);
    btnClose->Bind(wxEVT_BUTTON,    &DeparturePlanningDialog::OnClose, this);
    Bind(wxEVT_CLOSE_WINDOW, [this](wxCloseEvent&) { SaveConfig(); m_controller.FreeOverlays(); Hide(); });

    UpdateWeightControls();
    Layout();
}

// ---------------------------------------------------------------------------
// Base route
// ---------------------------------------------------------------------------

void DeparturePlanningDialog::UpdateBaseRoute() {
    RouteMapOverlay* base = m_wr.FirstCurrentRouteMap();
    if (!base) {
        m_stBaseRoute->SetLabel(_("(no route selected)"));
        m_btnRun->Disable();
        return;
    }
    RouteMapConfiguration cfg = base->GetConfiguration();
    wxString label = wxString::Format("Start %s → End %s",
        cfg.Start.c_str(), cfg.End.c_str());
    m_stBaseRoute->SetLabel(label);
    m_btnRun->Enable();
}

// ---------------------------------------------------------------------------
// Run / Stop
// ---------------------------------------------------------------------------

void DeparturePlanningDialog::OnRun(wxCommandEvent&) {
    if (m_controller.IsRunning()) {
        // Becomes Stop button while running.
        m_controller.StopSweep();
        m_btnRun->SetLabel(_("Run"));
        SetStatusText(_("Sweep cancelled — collecting results..."));
        return;
    }

    RouteMapOverlay* base = m_wr.FirstCurrentRouteMap();
    if (!base) {
        wxMessageBox(_("No route selected."), _("Departure Planning"), wxOK | wxICON_ERROR, this);
        return;
    }

    // Build sweep config.
    DepartureSweepController::SweepConfig cfg;
    cfg.base_config = base->GetConfiguration();

    // Read departure window from pickers (display tz → UTC).
    int tzOff = GetTzOffsetMin();
    wxDateTime depStart = m_dpStart->GetValue();
    wxTimeSpan ts = wxTimeSpan(m_tpStart->GetValue().GetHour(),
                               m_tpStart->GetValue().GetMinute(), 0);
    depStart += ts;
    depStart -= wxTimeSpan::Minutes(tzOff);  // to UTC

    wxDateTime depEnd = m_dpEnd->GetValue();
    ts = wxTimeSpan(m_tpEnd->GetValue().GetHour(), m_tpEnd->GetValue().GetMinute(), 0);
    depEnd += ts;
    depEnd -= wxTimeSpan::Minutes(tzOff);

    if (depEnd <= depStart) {
        wxMessageBox(_("Sweep end must be after sweep start."),
                     _("Departure Planning"), wxOK | wxICON_ERROR, this);
        return;
    }

    int sel = m_chStep->GetSelection();
    int step_s = (sel >= 0 && sel < (int)(sizeof(kStepSecs)/sizeof(kStepSecs[0])) - 1)
                 ? kStepSecs[sel] : 3600; // GRIB spacing auto → 1 h default
    if (step_s == 0) step_s = 3600;

    int delta_s = cfg.base_config.DeltaTime;
    if (step_s < delta_s) {
        wxMessageBox(
            wxString::Format(_("Step size (%d s) is less than DeltaTime (%d s)."), step_s, delta_s),
            _("Departure Planning"), wxOK | wxICON_ERROR, this);
        return;
    }

    // Use wide grib bounds (no pre-clamping).
    wxDateTime grib_s = depStart - wxTimeSpan::Days(1);
    wxDateTime grib_e = depEnd   + wxTimeSpan::Days(1);
    CandidateListResult clr = DepartureSweepController::BuildCandidateList(
        depStart, depEnd, grib_s, grib_e, step_s, delta_s);

    if (!clr.warning.IsEmpty()) {
        wxMessageBox(clr.warning, _("Departure Planning"), wxOK | wxICON_WARNING, this);
        return;
    }
    if (clr.departures.empty()) {
        wxMessageBox(_("No departure times generated."),
                     _("Departure Planning"), wxOK | wxICON_ERROR, this);
        return;
    }

    cfg.departures    = clr.departures;
    cfg.max_concurrent = 4;
    cfg.arrival_params = BuildArrivalParams();
    cfg.rank_params    = BuildRankParams();
    cfg.profile        = m_profile.IsLoaded() ? &m_profile : nullptr;

    m_lcResults->DeleteAllItems();
    m_sweepStartWall = wxDateTime::Now();
    m_controller.StartSweep(cfg);
    SetSweepRunning(true);
    m_gauge->SetRange((int)clr.departures.size());
    m_pollTimer.Start(kPollMs);
}

// ---------------------------------------------------------------------------
// Poll timer
// ---------------------------------------------------------------------------

void DeparturePlanningDialog::OnPollTimer(wxTimerEvent&) {
    int newDone = m_controller.Poll();
    if (newDone > 0)
        UpdateResultsList();

    int total  = m_controller.TotalCount();
    int done   = m_controller.CompletedCount();
    m_gauge->SetValue(done);

    wxTimeSpan elapsed = wxDateTime::Now() - m_sweepStartWall;
    SetStatusText(wxString::Format(_("%d of %d complete (elapsed: %s)"),
        done, total, elapsed.Format("%H:%M:%S")));

    if (!m_controller.IsRunning()) {
        m_pollTimer.Stop();
        UpdateResultsList();
        SetSweepRunning(false);
        if (m_controller.IsCancelled())
            SetStatusText(wxString::Format(
                _("Cancelled — %d of %d candidates complete."), done, total));
        else
            SetStatusText(wxString::Format(_("Sweep complete — %d candidates."), total));
    }
}

// ---------------------------------------------------------------------------
// Apply
// ---------------------------------------------------------------------------

void DeparturePlanningDialog::OnApply(wxCommandEvent&) {
    long idx = m_lcResults->GetNextItem(-1, wxLIST_NEXT_ALL, wxLIST_STATE_SELECTED);
    if (idx == wxNOT_FOUND) return;

    // Map visible index to sorted candidate index.
    const std::vector<SweepCandidate>& cands = m_controller.GetCandidates();
    if ((size_t)idx >= cands.size()) return;
    const SweepCandidate& sc = cands[idx];

    RouteMapOverlay* base = m_wr.FirstCurrentRouteMap();
    if (!base) return;

    RouteMapConfiguration cfg = base->GetConfiguration();
    cfg.StartTime      = sc.departure_utc;
    cfg.UseCurrentTime = false;
    base->SetConfiguration(cfg);

    SetStatusText(wxString::Format(_("Applied departure %s to base route."),
        DepartureSweepController::FormatDisplayTime(
            sc.departure_utc, GetTzOffsetMin())));
}

// ---------------------------------------------------------------------------
// Inspect
// ---------------------------------------------------------------------------

void DeparturePlanningDialog::OnInspect(wxCommandEvent&) {
    long idx = m_lcResults->GetNextItem(-1, wxLIST_NEXT_ALL, wxLIST_STATE_SELECTED);
    if (idx == wxNOT_FOUND) return;

    const std::vector<SweepCandidate>& cands = m_controller.GetCandidates();
    if ((size_t)idx >= cands.size()) return;
    if (!cands[idx].succeeded) return;

    RouteMapOverlay* ov = m_controller.GetOverlay((size_t)idx);
    if (!ov) return;

    std::list<RouteMapOverlay*> ovl = { ov };
    m_wr.GetStatisticsDialog().SetRouteMapOverlays(ovl);
    m_wr.GetStatisticsDialog().Show();
    m_wr.GetStatisticsDialog().Raise();

    m_wr.GetPlotDialog().SetRouteMapOverlay(ov);
    m_wr.GetPlotDialog().Show();
    m_wr.GetPlotDialog().Raise();
}

// ---------------------------------------------------------------------------
// Close
// ---------------------------------------------------------------------------

void DeparturePlanningDialog::OnClose(wxCommandEvent&) {
    SaveConfig();
    m_pollTimer.Stop();
    m_controller.FreeOverlays();
    Hide();
}

// ---------------------------------------------------------------------------
// Profile browse
// ---------------------------------------------------------------------------

void DeparturePlanningDialog::OnBrowseProfile(wxCommandEvent&) {
    wxFileDialog dlg(this, _("Open Comfort Profile"), wxEmptyString, wxEmptyString,
                     _("JSON files (*.json)|*.json|All files|*.*"),
                     wxFD_OPEN | wxFD_FILE_MUST_EXIST);
    if (dlg.ShowModal() != wxID_OK) return;

    wxString path = dlg.GetPath();
    std::string err;
    if (m_profile.Load(path.ToStdString(), err)) {
        m_tcProfilePath->SetValue(path);
        m_stProfileStatus->SetLabel(m_profile.StatusLabel());
    } else {
        m_stProfileStatus->SetLabel(wxString("Invalid profile: ") + err);
        m_tcProfilePath->SetValue(wxEmptyString);
    }
    Layout();
}

// ---------------------------------------------------------------------------
// UI helpers
// ---------------------------------------------------------------------------

void DeparturePlanningDialog::OnUtcLocal(wxCommandEvent&) {
    bool local = m_rbLocal->GetValue();
    m_spTzHour->Enable(local);
    m_spTzMin->Enable(local);
    m_stTzSign->Enable(local);
    UpdateColumnHeaders();
}

void DeparturePlanningDialog::OnArrivalFilterCheck(wxCommandEvent&) {
    m_pnlArrivalControls->Enable(m_cbArrival->IsChecked());
    // Re-rank with updated filter if sweep is done.
    if (!m_controller.IsRunning() && !m_controller.GetCandidates().empty()) {
        m_controller.ReapplyArrivalFilter(BuildArrivalParams(), BuildRankParams());
        UpdateResultsList();
    }
}

void DeparturePlanningDialog::OnArrivalModeChange(wxCommandEvent&) {
    UpdateArrivalControls();
    if (!m_controller.IsRunning() && !m_controller.GetCandidates().empty()) {
        m_controller.ReapplyArrivalFilter(BuildArrivalParams(), BuildRankParams());
        UpdateResultsList();
    }
}

void DeparturePlanningDialog::OnRankModeChange(wxCommandEvent&) {
    UpdateWeightControls();
    if (!m_controller.IsRunning() && !m_controller.GetCandidates().empty()) {
        m_controller.RerankCandidates(BuildRankParams());
        UpdateResultsList();
    }
}

void DeparturePlanningDialog::OnRankWeightChange(wxCommandEvent&) {
    double w = m_slWeight->GetValue() / 10.0;
    m_stWeight->SetLabel(wxString::Format("%.1f", w));
    if (!m_controller.IsRunning() && !m_controller.GetCandidates().empty()) {
        m_controller.RerankCandidates(BuildRankParams());
        UpdateResultsList();
    }
}

void DeparturePlanningDialog::UpdateArrivalControls() {
    bool fixed = m_rbFixed->GetValue();
    m_pnlFixed->Show(fixed);
    m_pnlSunset->Show(!fixed);
    m_pnlArrivalControls->Layout();
}

void DeparturePlanningDialog::UpdateWeightControls() {
    int sel = m_chRankMode->GetSelection();
    bool balanced = (sel == 0); // "Balanced" is index 0
    m_pnlWeight->Show(balanced);
    Layout();
}

void DeparturePlanningDialog::UpdateColumnHeaders() {
    wxString tzLabel = DepartureSweepController::FormatTzLabel(GetTzOffsetMin());
    m_lcResults->SetColumnWidth(COL_DEP, 145);
    m_lcResults->SetColumnWidth(COL_ETA, 145);
    wxListItem col;
    col.SetMask(wxLIST_MASK_TEXT);
    col.SetId(COL_DEP); col.SetText(_("Departure (") + tzLabel + ")");
    m_lcResults->SetColumn(COL_DEP, col);
    col.SetId(COL_ETA); col.SetText(_("ETA (") + tzLabel + ")");
    m_lcResults->SetColumn(COL_ETA, col);
}

void DeparturePlanningDialog::SetSweepRunning(bool running) {
    m_btnRun->SetLabel(running ? _("Cancel") : _("Run"));
    m_gauge->Show(running);
    m_btnApply->Enable(!running);
    m_btnInspect->Enable(!running);
    Layout();
}

void DeparturePlanningDialog::SetStatusText(const wxString& msg) {
    m_stStatus->SetLabel(msg);
}

// ---------------------------------------------------------------------------
// Results list
// ---------------------------------------------------------------------------

void DeparturePlanningDialog::UpdateResultsList() {
    m_lcResults->Freeze();
    m_lcResults->DeleteAllItems();

    const std::vector<SweepCandidate>& cands = m_controller.GetCandidates();
    bool filterEnabled = m_cbArrival->IsChecked();
    int tzOff = GetTzOffsetMin();

    // Show/hide Arrival column.
    if (filterEnabled != m_arrivalColVisible) {
        m_arrivalColVisible = filterEnabled;
        m_lcResults->SetColumnWidth(COL_ARRIVAL, filterEnabled ? 85 : 0);
    }

    for (size_t i = 0; i < cands.size(); ++i) {
        const SweepCandidate& c = cands[i];
        long row = m_lcResults->InsertItem((long)i, wxEmptyString);

        // Rank
        if (c.rank > 0)
            m_lcResults->SetItem(row, COL_RANK, wxString::Format("%d", c.rank));
        else
            m_lcResults->SetItem(row, COL_RANK, "—");

        // Departure
        m_lcResults->SetItem(row, COL_DEP,
            DepartureSweepController::FormatDisplayTime(c.departure_utc, tzOff));

        if (c.succeeded) {
            // ETA
            m_lcResults->SetItem(row, COL_ETA,
                DepartureSweepController::FormatDisplayTime(c.eta_utc, tzOff));
            // Duration
            m_lcResults->SetItem(row, COL_DUR, FmtDuration(c.duration));
            // Avg TWS
            m_lcResults->SetItem(row, COL_AVGTWS, FmtDouble1(c.avg_tws_kn));
            // Max TWS
            m_lcResults->SetItem(row, COL_MAXTWS, FmtDouble1(c.max_tws_kn));
            // Max Swell
            m_lcResults->SetItem(row, COL_SWELL,
                std::isnan(c.max_swell_m)
                ? wxString("n/a")
                : wxString::Format("%.1f", c.max_swell_m));
            // Comfort
            m_lcResults->SetItem(row, COL_COMFORT,
                std::isnan(c.comfort_penalty)
                ? wxString("n/a")
                : wxString::Format("%.2f", c.comfort_penalty));
            // Arrival
            if (filterEnabled) {
                m_lcResults->SetItem(row, COL_ARRIVAL,
                    c.arrival_ok ? wxString("✓") : c.arrival_miss);
            }
            // Status
            m_lcResults->SetItem(row, COL_STATUS, "OK");

            // Tier 2 styling: italic (approximate with different colour)
            if (!c.arrival_ok && filterEnabled) {
                wxListItem li;
                li.SetId(row);
                li.SetTextColour(wxColour(80, 80, 80));
                m_lcResults->SetItem(li);
            }
        } else {
            m_lcResults->SetItem(row, COL_ETA,    "—");
            m_lcResults->SetItem(row, COL_DUR,    "—");
            m_lcResults->SetItem(row, COL_AVGTWS, "—");
            m_lcResults->SetItem(row, COL_MAXTWS, "—");
            m_lcResults->SetItem(row, COL_SWELL,  "—");
            m_lcResults->SetItem(row, COL_COMFORT,"—");
            if (filterEnabled)
                m_lcResults->SetItem(row, COL_ARRIVAL, "—");
            m_lcResults->SetItem(row, COL_STATUS,
                c.failure_reason.IsEmpty() ? wxString("Failed") : c.failure_reason);

            // Tier 3: dimmed
            wxListItem li;
            li.SetId(row);
            li.SetTextColour(*wxLIGHT_GREY);
            m_lcResults->SetItem(li);
        }
    }

    m_lcResults->Thaw();

    // Enable Apply / Inspect based on selection.
    bool running = m_controller.IsRunning();
    m_btnApply->Enable(!running);
    m_btnInspect->Enable(!running);
}

// ---------------------------------------------------------------------------
// Accessors / builders
// ---------------------------------------------------------------------------

int DeparturePlanningDialog::GetTzOffsetMin() const {
    if (!m_rbLocal->GetValue()) return 0;
    int h = m_spTzHour->GetValue();
    int m = m_spTzMin->GetValue();
    // Sign: positive = ahead of UTC
    return h * 60 + m;
}

int DeparturePlanningDialog::GetArrivalEarliestUtc() const {
    int disp = m_tpEarliest->GetValue().GetHour() * 60
             + m_tpEarliest->GetValue().GetMinute();
    int tz   = GetTzOffsetMin();
    return ((disp - tz) % 1440 + 1440) % 1440;
}

int DeparturePlanningDialog::GetArrivalLatestUtc() const {
    int disp = m_tpLatest->GetValue().GetHour() * 60
             + m_tpLatest->GetValue().GetMinute();
    int tz   = GetTzOffsetMin();
    return ((disp - tz) % 1440 + 1440) % 1440;
}

ArrivalWindowParams DeparturePlanningDialog::BuildArrivalParams() const {
    ArrivalWindowParams p;
    p.enabled              = m_cbArrival->IsChecked();
    p.mode                 = m_rbFixed->GetValue()
                             ? ArrivalFilterMode::FIXED_WINDOW
                             : ArrivalFilterMode::BEFORE_SUNSET;
    p.arrival_earliest_utc = GetArrivalEarliestUtc();
    p.arrival_latest_utc   = GetArrivalLatestUtc();
    p.sunset_margin_h      = m_spMargin->GetValue();
    p.display_tz_offset_min = const_cast<DeparturePlanningDialog*>(this)->GetTzOffsetMin();

    // Destination coords from base route.
    RouteMapOverlay* base = m_wr.FirstCurrentRouteMap();
    if (base) {
        RouteMapConfiguration cfg = base->GetConfiguration();
        p.end_lat = cfg.EndLat;
        p.end_lon = cfg.EndLon;
    }
    return p;
}

RankParams DeparturePlanningDialog::BuildRankParams() const {
    RankParams rp;
    int sel = m_chRankMode->GetSelection();
    if (sel == 1) rp.mode = SortMode::FASTEST;
    else if (sel == 2) rp.mode = SortMode::SMOOTHEST;
    else rp.mode = SortMode::BALANCED;
    rp.balanced_weight = m_slWeight->GetValue() / 10.0;
    return rp;
}

// ---------------------------------------------------------------------------
// Config persistence
// ---------------------------------------------------------------------------

void DeparturePlanningDialog::LoadConfig() {
    wxFileConfig* cfg = dynamic_cast<wxFileConfig*>(wxConfigBase::Get());
    if (!cfg) return;
    wxString old_path;
    old_path = cfg->GetPath();
    cfg->SetPath("/PlugIns/WeatherRouting/DeparturePlanning");

    int tz = cfg->ReadLong("DisplayTzOffsetMin", 0);
    if (tz != 0) {
        m_rbLocal->SetValue(true);
        m_spTzHour->SetValue(std::abs(tz) / 60);
        m_spTzMin->SetValue(std::abs(tz) % 60);
        m_spTzHour->Enable();
        m_spTzMin->Enable();
        m_stTzSign->Enable();
    }

    int rankMode = cfg->ReadLong("RankMode", 0);
    m_chRankMode->SetSelection(rankMode);

    int weight10 = cfg->ReadLong("BalancedWeight10", 5);
    m_slWeight->SetValue(weight10);
    m_stWeight->SetLabel(wxString::Format("%.1f", weight10 / 10.0));

    wxString profPath;
    if (cfg->Read("ComfortProfilePath", &profPath) && !profPath.IsEmpty()) {
        std::string err;
        if (m_profile.Load(profPath.ToStdString(), err)) {
            m_tcProfilePath->SetValue(profPath);
            m_stProfileStatus->SetLabel(m_profile.StatusLabel());
        }
    }

    cfg->SetPath(old_path);
    UpdateWeightControls();
    UpdateColumnHeaders();
}

void DeparturePlanningDialog::SaveConfig() {
    wxFileConfig* cfg = dynamic_cast<wxFileConfig*>(wxConfigBase::Get());
    if (!cfg) return;
    wxString old_path = cfg->GetPath();
    cfg->SetPath("/PlugIns/WeatherRouting/DeparturePlanning");

    int tz = GetTzOffsetMin();
    cfg->Write("DisplayTzOffsetMin",  (long)tz);
    cfg->Write("RankMode",            (long)m_chRankMode->GetSelection());
    cfg->Write("BalancedWeight10",    (long)m_slWeight->GetValue());
    cfg->Write("ComfortProfilePath",  m_tcProfilePath->GetValue());

    cfg->SetPath(old_path);
    cfg->Flush();
}

// ---------------------------------------------------------------------------
// Formatting helpers
// ---------------------------------------------------------------------------

wxString DeparturePlanningDialog::FmtDuration(const wxTimeSpan& ts) {
    long secs  = ts.GetSeconds().ToLong();
    if (secs < 0) return "—";
    long days  = secs / 86400;
    long hours = (secs % 86400) / 3600;
    long mins  = (secs % 3600)  / 60;
    if (days > 0)
        return wxString::Format("%ldd %02ld:%02ld", days, hours, mins);
    return wxString::Format("%02ld:%02ld", hours, mins);
}

wxString DeparturePlanningDialog::FmtDouble1(double v, const wxString& na_str) {
    if (std::isnan(v)) return na_str;
    return wxString::Format("%.1f", v);
}
