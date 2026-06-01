/*
 * DeparturePlanningDialog.h — WR.3 Departure Planning Dialog
 *
 * Non-modal wxDialog that runs a departure-time sweep over the currently
 * selected WRPI route, ranks results, and lets the user Apply a chosen
 * departure to the base configuration.
 *
 * Contract: docs/DEPARTURE_SWEEP_CONTRACT_FINAL-1.md §5.
 */
#pragma once
#include <wx/wx.h>
#include <wx/datectrl.h>
#include <wx/timectrl.h>
#include <wx/dateevt.h>
#include <wx/listctrl.h>
#include <wx/spinctrl.h>
#include <wx/slider.h>
#include <wx/gauge.h>
#include <wx/timer.h>

#include "DepartureSweepController.h"
#include "ComfortProfileLoader.h"

class WeatherRouting;

class DeparturePlanningDialog : public wxDialog {
public:
    explicit DeparturePlanningDialog(wxWindow* parent, WeatherRouting& wr);
    ~DeparturePlanningDialog();

    // Call from WeatherRouting when the base-route selection changes.
    void UpdateBaseRoute();

private:
    // --- Event handlers ---
    void OnRun(wxCommandEvent&);
    void OnStop(wxCommandEvent&);
    void OnApply(wxCommandEvent&);
    void OnInspect(wxCommandEvent&);
    void OnClose(wxCommandEvent&);
    void OnBrowseProfile(wxCommandEvent&);
    void OnPollTimer(wxTimerEvent&);
    void OnUtcLocal(wxCommandEvent&);
    void OnTzOffsetChange(wxSpinEvent&);
    void OnArrivalFilterCheck(wxCommandEvent&);
    void OnArrivalModeChange(wxCommandEvent&);
    void OnRankModeChange(wxCommandEvent&);
    void OnRankWeightChange(wxCommandEvent&);

    // --- Column enum ---
    enum Col {
        COL_RANK = 0, COL_DEP, COL_ETA, COL_DUR,
        COL_AVGTWS, COL_MAXTWS, COL_SWELL, COL_COMFORT,
        COL_ARRIVAL, COL_STATUS,
        COL_COUNT
    };

    // --- Helpers ---
    void BuildUI();
    void UnpinInspect(); // release PlotDialog pin, safe to call at any time
    void UpdateColumnHeaders();
    void UpdateResultsList();
    void SetSweepRunning(bool running);
    void UpdateArrivalControls();
    void UpdateWeightControls();
    void LoadConfig();
    void SaveConfig();
    int  GetTzOffsetMin() const;
    int  GetArrivalEarliestUtc() const;
    int  GetArrivalLatestUtc() const;
    ArrivalWindowParams BuildArrivalParams() const;
    RankParams          BuildRankParams() const;
    void SetStatusText(const wxString& msg);

    static wxString FmtDuration(const wxTimeSpan& ts);
    static wxString FmtDouble1(double v, const wxString& na_str = "n/a");

    // --- State ---
    WeatherRouting&             m_wr;
    DepartureSweepController    m_controller;
    ComfortProfileLoader        m_profile;
    wxTimer                     m_pollTimer;
    wxDateTime                  m_sweepStartWall; // for elapsed display
    bool                        m_arrivalColVisible = false;
    bool                        m_inspecting        = false;

    // --- Controls ---
    wxStaticText*    m_stBaseRoute;
    wxRadioButton*   m_rbUtc;
    wxRadioButton*   m_rbLocal;
    wxSpinCtrl*      m_spTzHour;
    wxSpinCtrl*      m_spTzMin;
    wxStaticText*    m_stTzSign;
    wxDatePickerCtrl* m_dpStart;
    wxTimePickerCtrl* m_tpStart;
    wxDatePickerCtrl* m_dpEnd;
    wxTimePickerCtrl* m_tpEnd;
    wxChoice*        m_chStep;
    wxChoice*        m_chRankMode;
    wxSlider*        m_slWeight;
    wxStaticText*    m_stWeight;
    wxPanel*         m_pnlWeight;
    wxTextCtrl*      m_tcProfilePath;
    wxStaticText*    m_stProfileStatus;
    wxCheckBox*      m_cbArrival;
    wxRadioButton*   m_rbFixed;
    wxRadioButton*   m_rbSunset;
    wxTimePickerCtrl* m_tpEarliest;
    wxTimePickerCtrl* m_tpLatest;
    wxSpinCtrlDouble* m_spMargin;
    wxPanel*         m_pnlFixed;
    wxPanel*         m_pnlSunset;
    wxPanel*         m_pnlArrivalControls;
    wxListCtrl*      m_lcResults;
    wxGauge*         m_gauge;
    wxStaticText*    m_stStatus;
    wxButton*        m_btnRun;
    wxButton*        m_btnApply;
    wxButton*        m_btnInspect;

    static const int kPollMs = 250;
};
