//------------------------------------------------------------------------------
// <copyright file="BodyBasics.cpp" company="Microsoft">
//     Copyright (c) Microsoft Corporation.  All rights reserved.
// </copyright>
//------------------------------------------------------------------------------

#include "stdafx.h"
#include <strsafe.h>
#include "resource.h"
#include "fileapi.h"
#include "BodyBasics.h"
#include "wincodec.h"
#include <windows.h>
#include <d2d1.h>
#include <d2d1helper.h>
#include <dwrite.h>
#include <chrono>
#pragma comment(lib, "dwrite.lib")
#pragma comment(lib, "d2d1.lib")
#pragma comment(lib, "dxguid.lib")
#pragma comment(lib, "dxgi.lib")
#include <iostream>
#include <fstream>
using namespace std;
#define PI 3.14159265
#define Upright 1
#define Leaning_rapidly 2
#define Leaning_safely 3
#define Low 4
#define Safe_Low 5
#define Fall 6
// Threshold
#define Height_Thres_Up_or_L 1
#define Fall_speed_Thres 0.005
#define Lean_speed_Thres 5
#define Change_speed_Thres 25
#define Doubt_Count_Thres_LR_to_LS 5
#define Doubt_Count_Thres_LS_to_LR 5
#define FALL_DETECTION_THRES 3


static const float c_JointThickness = 3.0f;
static const float c_TrackedBoneThickness = 6.0f;
static const float c_InferredBoneThickness = 1.0f;
static const float c_HandSize = 30.0f;

/// <summary>
/// Entry point for the application
/// </summary>
/// <param name="hInstance">handle to the application instance</param>
/// <param name="hPrevInstance">always 0</param>
/// <param name="lpCmdLine">command line arguments</param>
/// <param name="nCmdShow">whether to display minimized, maximized, or normally</param>
/// <returns>status</returns>
int APIENTRY wWinMain(
	_In_ HINSTANCE hInstance,
	_In_opt_ HINSTANCE hPrevInstance,
	_In_ LPWSTR lpCmdLine,
	_In_ int nShowCmd
)
{
	UNREFERENCED_PARAMETER(hPrevInstance);
	UNREFERENCED_PARAMETER(lpCmdLine);

	CBodyBasics application;
	//OutputDebugPrintf("HI");
	application.Run(hInstance, nShowCmd);
}

/// <summary>
/// Constructor
/// </summary>
CBodyBasics::CBodyBasics() :
	m_hWnd(NULL),
	m_nStartTime(0),
	m_nLastCounter(0),
	m_nFramesSinceUpdate(0),
	m_fFreq(0),
	m_nNextStatusTime(0LL),
	m_pKinectSensor(NULL),
	m_pCoordinateMapper(NULL),
	m_pBodyFrameReader(NULL),
	m_pD2DFactory(NULL),
	m_pRenderTarget(NULL),
	m_pBrushJointTracked(NULL),
	m_pBrushJointInferred(NULL),
	m_pBrushBoneTracked(NULL),
	m_pBrushBoneInferred(NULL),
	m_pBrushHandClosed(NULL),
	m_pBrushHandOpen(NULL),
	m_pBrushHandLasso(NULL),
	m_pBrushText(NULL),
    Floor_Depth_Updating(FALSE),
    Floor_Depth_Updating_state(0),
    Floor_Depth_Updating_threshold(3),
	Begin_State(0),
	Begin_threshold(3),
	Depth_Computing_Sum(0),
	Pos_Height_Computing_Sum(0)
{
	LARGE_INTEGER qpf = { 0 };
	if (QueryPerformanceFrequency(&qpf))
	{
		m_fFreq = double(qpf.QuadPart);
	}
}


/// <summary>
/// Destructor
/// </summary>
CBodyBasics::~CBodyBasics()
{
	DiscardDirect2DResources();

	// clean up Direct2D
	SafeRelease(m_pD2DFactory);

	// done with body frame reader
	SafeRelease(m_pBodyFrameReader);

	// done with coordinate mapper
	SafeRelease(m_pCoordinateMapper);

	// close the Kinect Sensor
	if (m_pKinectSensor)
	{
		m_pKinectSensor->Close();
	}

	SafeRelease(m_pKinectSensor);
}

/// <summary>
/// Creates the main window and begins processing
/// </summary>
/// <param name="hInstance">handle to the application instance</param>
/// <param name="nCmdShow">whether to display minimized, maximized, or normally</param>
int CBodyBasics::Run(HINSTANCE hInstance, int nCmdShow)
{
	MSG       msg = { 0 };
	WNDCLASS  wc;

	// File Operation
	std::ofstream dataFile;
	dataFile.open("safe_new.csv", std::ofstream::out | std::ofstream::app);
	dataFile << "height" << "," << "leftHipAngle" << "," << "rightHipAngle" << ",";
	dataFile << "leftKneeAngle" << "," << "rightKneeAngle" << "," << "chestAngle" << ",";
	dataFile << "chestKneeAngle" << "," << "time" << "\n";;
	dataFile.close();

	// Dialog custom window class
	ZeroMemory(&wc, sizeof(wc));
	wc.style = CS_HREDRAW | CS_VREDRAW;
	wc.cbWndExtra = DLGWINDOWEXTRA;
	wc.hCursor = LoadCursorW(NULL, IDC_ARROW);
	wc.hIcon = LoadIconW(hInstance, MAKEINTRESOURCE(IDI_APP));
	wc.lpfnWndProc = DefDlgProcW;
	wc.lpszClassName = L"BodyBasicsAppDlgWndClass";

	if (!RegisterClassW(&wc))
	{
		return 0;
	}

	// Create main application window
	HWND hWndApp = CreateDialogParamW(
		NULL,
		MAKEINTRESOURCE(IDD_APP),
		NULL,
		(DLGPROC)CBodyBasics::MessageRouter,
		reinterpret_cast<LPARAM>(this));

	// Show window
	ShowWindow(hWndApp, nCmdShow);

	// Main message loop
	while (WM_QUIT != msg.message)
	{
		Update();

		while (PeekMessageW(&msg, NULL, 0, 0, PM_REMOVE))
		{
			// If a dialog message will be taken care of by the dialog proc
			if (hWndApp && IsDialogMessageW(hWndApp, &msg))
			{
				continue;
			}

			TranslateMessage(&msg);
			DispatchMessageW(&msg);
		}
	}

	return static_cast<int>(msg.wParam);
}

/// <summary>
/// Main processing function
/// </summary>
void CBodyBasics::Update()
{
	if (!m_pBodyFrameReader)
	{
		return;
	}

	IBodyFrame* pBodyFrame = NULL;

	HRESULT hr = m_pBodyFrameReader->AcquireLatestFrame(&pBodyFrame);

	if (SUCCEEDED(hr))
	{
		INT64 nTime = 0;

		hr = pBodyFrame->get_RelativeTime(&nTime);

		IBody* ppBodies[BODY_COUNT] = { 0 };

		if (SUCCEEDED(hr))
		{
			hr = pBodyFrame->GetAndRefreshBodyData(_countof(ppBodies), ppBodies);
		}

		if (SUCCEEDED(hr))
		{
			ProcessBody(nTime, BODY_COUNT, ppBodies);
		}

		for (int i = 0; i < _countof(ppBodies); ++i)
		{
			SafeRelease(ppBodies[i]);
		}
	}

	SafeRelease(pBodyFrame);
}

/// <summary>
/// Handles window messages, passes most to the class instance to handle
/// </summary>
/// <param name="hWnd">window message is for</param>
/// <param name="uMsg">message</param>
/// <param name="wParam">message data</param>
/// <param name="lParam">additional message data</param>
/// <returns>result of message processing</returns>
LRESULT CALLBACK CBodyBasics::MessageRouter(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam)
{
	CBodyBasics* pThis = NULL;

	if (WM_INITDIALOG == uMsg)
	{
		pThis = reinterpret_cast<CBodyBasics*>(lParam);
		SetWindowLongPtr(hWnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(pThis));
	}
	else
	{
		pThis = reinterpret_cast<CBodyBasics*>(::GetWindowLongPtr(hWnd, GWLP_USERDATA));
	}

	if (pThis)
	{
		return pThis->DlgProc(hWnd, uMsg, wParam, lParam);
	}

	return 0;
}

/// <summary>
/// Handle windows messages for the class instance
/// </summary>
/// <param name="hWnd">window message is for</param>
/// <param name="uMsg">message</param>
/// <param name="wParam">message data</param>
/// <param name="lParam">additional message data</param>
/// <returns>result of message processing</returns>
LRESULT CALLBACK CBodyBasics::DlgProc(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam)
{
	UNREFERENCED_PARAMETER(wParam);
	UNREFERENCED_PARAMETER(lParam);

	switch (message)
	{
	case WM_INITDIALOG:
	{
		// Bind application window handle
		m_hWnd = hWnd;

		// Init Direct2D
		D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, &m_pD2DFactory);

		// Get and initialize the default Kinect sensor
		InitializeDefaultSensor();
	}
	break;

	// If the titlebar X is clicked, destroy app
	case WM_CLOSE:
		DestroyWindow(hWnd);
		break;

	case WM_DESTROY:
		// Quit the main message pump
		PostQuitMessage(0);
		break;
	}

	return FALSE;
}

/// <summary>
/// Initializes the default Kinect sensor
/// </summary>
/// <returns>indicates success or failure</returns>
HRESULT CBodyBasics::InitializeDefaultSensor()
{
	HRESULT hr;

	hr = GetDefaultKinectSensor(&m_pKinectSensor);
	if (FAILED(hr))
	{
		return hr;
	}

	if (m_pKinectSensor)
	{
		// Initialize the Kinect and get coordinate mapper and the body reader
		IBodyFrameSource* pBodyFrameSource = NULL;

		hr = m_pKinectSensor->Open();

		if (SUCCEEDED(hr))
		{
			hr = m_pKinectSensor->get_CoordinateMapper(&m_pCoordinateMapper);
		}

		if (SUCCEEDED(hr))
		{
			hr = m_pKinectSensor->get_BodyFrameSource(&pBodyFrameSource);
		}

		if (SUCCEEDED(hr))
		{
			hr = pBodyFrameSource->OpenReader(&m_pBodyFrameReader);
		}

		SafeRelease(pBodyFrameSource);
	}

	if (!m_pKinectSensor || FAILED(hr))
	{
		SetStatusMessage(L"No ready Kinect found!", 10000, true);
		return E_FAIL;
	}

	return hr;
}

/// <summary>
/// Handle new body data
/// <param name="nTime">timestamp of frame</param>
/// <param name="nBodyCount">body data count</param>
/// <param name="ppBodies">body data in frame</param>
/// </summary>
void CBodyBasics::ProcessBody(INT64 nTime, int nBodyCount, IBody** ppBodies)
{
	if (m_hWnd)
	{
		HRESULT hr = EnsureDirect2DResources();

		if (SUCCEEDED(hr) && m_pRenderTarget && m_pCoordinateMapper)
		{
			m_pRenderTarget->BeginDraw();
			m_pRenderTarget->Clear();

			IDWriteFactory* g_pDWriteFactory = NULL;

			// Create DirectWrite Factory
			hr = DWriteCreateFactory(
				DWRITE_FACTORY_TYPE_SHARED,
				__uuidof(IDWriteFactory),
				reinterpret_cast<IUnknown**>(&g_pDWriteFactory)
			);
			if (FAILED(hr))
			{
				MessageBox(NULL, L"Create DirectWrite factory failed!", L"Error", 0);
				return;
			}

			IDWriteTextFormat* g_pTextFormat = NULL;

			hr = g_pDWriteFactory->CreateTextFormat(
				L"Gabriola",                   // Font family name
				NULL,                          // Font collection(NULL sets it to the system font collection)
				DWRITE_FONT_WEIGHT_REGULAR,    // Weight
				DWRITE_FONT_STYLE_NORMAL,      // Style
				DWRITE_FONT_STRETCH_NORMAL,    // Stretch
				50.0f,                         // Size    
				L"en-us",                      // Local
				&g_pTextFormat                 // Pointer to recieve the created object
			);
			if (FAILED(hr))
			{
				MessageBox(NULL, L"Create IDWriteTextFormat failed!", L"Error", 0);
				return;
			}

			// Create text layout rect
			RECT rc;
			GetClientRect(m_hWnd, &rc);
			D2D1_RECT_F textLayoutRect = D2D1::RectF(
				static_cast<FLOAT>(rc.left),
				static_cast<FLOAT>(rc.top),
				static_cast<FLOAT>(rc.right - rc.left),
				static_cast<FLOAT>(rc.bottom - rc.top)
			);


			// Create Brush
			m_pRenderTarget->CreateSolidColorBrush(
				D2D1::ColorF(D2D1::ColorF::LightPink),
				&m_pBrushText
			);



			RECT rct;
			GetClientRect(GetDlgItem(m_hWnd, IDC_VIDEOVIEW), &rct);
			int width = rct.right;
			int height = rct.bottom;

			std::ofstream dataFile;
			dataFile.open("safe_new.csv", std::ofstream::out | std::ofstream::app);


			for (int i = 0; i < nBodyCount; ++i)
			{
				IBody* pBody = ppBodies[i];
				if (pBody)
				{
					BOOLEAN bTracked = false;
					hr = pBody->get_IsTracked(&bTracked);

					if (SUCCEEDED(hr) && bTracked)
					{
						Joint joints[JointType_Count];
						D2D1_POINT_2F jointPoints[JointType_Count];
						HandState leftHandState = HandState_Unknown;
						HandState rightHandState = HandState_Unknown;

						pBody->get_HandLeftState(&leftHandState);
						pBody->get_HandRightState(&rightHandState);

						hr = pBody->GetJoints(_countof(joints), joints);
						if (SUCCEEDED(hr))
						{
							//Get Body Point Position
							const CameraSpacePoint &spineBasePos = joints[JointType_SpineBase].Position;
							const CameraSpacePoint &spineMidPos = joints[JointType_SpineMid].Position;
							const CameraSpacePoint &neckPos = joints[JointType_Neck].Position;
							const CameraSpacePoint &headPos = joints[JointType_Head].Position;
							const CameraSpacePoint &shoulderLeftPos = joints[JointType_ShoulderLeft].Position;

							const CameraSpacePoint &elbowLeftPos = joints[JointType_ElbowLeft].Position;
							const CameraSpacePoint &wristLeftPos = joints[JointType_WristLeft].Position;
							const CameraSpacePoint &handLeftPos = joints[JointType_HandLeft].Position;
							const CameraSpacePoint &shoulderRightPos = joints[JointType_ShoulderRight].Position;
							const CameraSpacePoint &elbowRightPos = joints[JointType_ElbowRight].Position;

							const CameraSpacePoint &wristRightPos = joints[JointType_WristRight].Position;
							const CameraSpacePoint &handRightPos = joints[JointType_HandRight].Position;
							const CameraSpacePoint &hipLeftPos = joints[JointType_HipLeft].Position;
							const CameraSpacePoint &kneeLeftPos = joints[JointType_KneeLeft].Position;
							const CameraSpacePoint &ankleLeftPos = joints[JointType_AnkleLeft].Position;

							const CameraSpacePoint &footLeftPos = joints[JointType_FootLeft].Position;
							const CameraSpacePoint &hipRightPos = joints[JointType_HipRight].Position;
							const CameraSpacePoint &kneeRightPos = joints[JointType_KneeRight].Position;
							const CameraSpacePoint &ankleRightPos = joints[JointType_AnkleRight].Position;
							const CameraSpacePoint &footRightPos = joints[JointType_FootRight].Position;

							const CameraSpacePoint &spineShoulderPos = joints[JointType_SpineShoulder].Position;
							const CameraSpacePoint &handTipLeftPos = joints[JointType_HandTipLeft].Position;
							const CameraSpacePoint &thumbLeftPos = joints[JointType_ThumbLeft].Position;
							const CameraSpacePoint &handTipRightPos = joints[JointType_HandTipRight].Position;
							const CameraSpacePoint &thumbRightPos = joints[JointType_ThumbRight].Position;

							//distances between joints
							float a = sqrt(pow(hipLeftPos.X - kneeLeftPos.X, 2) + pow(hipLeftPos.Y - kneeLeftPos.Y, 2) + pow(hipLeftPos.Z - kneeLeftPos.Z, 2));
							float b = sqrt(pow(spineBasePos.X - hipLeftPos.X, 2) + pow(spineBasePos.Y - hipLeftPos.Y, 2) + pow(spineBasePos.Z - hipLeftPos.Z, 2));
							float c = sqrt(pow(spineBasePos.X - kneeLeftPos.X, 2) + pow(spineBasePos.Y - kneeLeftPos.Y, 2) + pow(spineBasePos.Z - kneeLeftPos.Z, 2));

							float d = sqrt(pow(hipRightPos.X - kneeRightPos.X, 2) + pow(hipRightPos.Y - kneeRightPos.Y, 2) + pow(hipRightPos.Z - kneeRightPos.Z, 2));
							float e = sqrt(pow(spineBasePos.X - hipRightPos.X, 2) + pow(spineBasePos.Y - hipRightPos.Y, 2) + pow(spineBasePos.Z - hipRightPos.Z, 2));
							float f = sqrt(pow(spineBasePos.X - kneeRightPos.X, 2) + pow(spineBasePos.Y - kneeRightPos.Y, 2) + pow(spineBasePos.Z - kneeRightPos.Z, 2));

							float g = sqrt(pow(hipLeftPos.X - ankleLeftPos.X, 2) + pow(hipLeftPos.Y - ankleLeftPos.Y, 2) + pow(hipLeftPos.Z - ankleLeftPos.Z, 2));
							float h = sqrt(pow(kneeLeftPos.X - ankleLeftPos.X, 2) + pow(kneeLeftPos.Y - ankleLeftPos.Y, 2) + pow(kneeLeftPos.Z - ankleLeftPos.Z, 2));

							float i = sqrt(pow(hipRightPos.X - ankleRightPos.X, 2) + pow(hipRightPos.Y - ankleRightPos.Y, 2) + pow(hipRightPos.Z - ankleRightPos.Z, 2));
							float j = sqrt(pow(kneeRightPos.X - ankleRightPos.X, 2) + pow(kneeRightPos.Y - ankleRightPos.Y, 2) + pow(kneeRightPos.Z - ankleRightPos.Z, 2));

							float k = sqrt(pow(kneeLeftPos.X - footLeftPos.X, 2) + pow(kneeLeftPos.Y - footLeftPos.Y, 2) + pow(kneeLeftPos.Z - footLeftPos.Z, 2));
							float l = sqrt(pow(ankleLeftPos.X - footLeftPos.X, 2) + pow(ankleLeftPos.Y - footLeftPos.Y, 2) + pow(ankleLeftPos.Z - footLeftPos.Z, 2));

							float m = sqrt(pow(kneeRightPos.X - footRightPos.X, 2) + pow(kneeRightPos.Y - footRightPos.Y, 2) + pow(kneeRightPos.Z - footRightPos.Z, 2));
							float n = sqrt(pow(ankleRightPos.X - footRightPos.X, 2) + pow(ankleRightPos.Y - footRightPos.Y, 2) + pow(ankleRightPos.Z - footRightPos.Z, 2));

							float o = sqrt(pow((0.5*ankleLeftPos.X + 0.5*ankleRightPos.X) - footLeftPos.X, 2) + pow(ankleLeftPos.Z - footLeftPos.Z, 2));
							float p = sqrt(pow((0.5*ankleLeftPos.X + 0.5*ankleRightPos.X) - footRightPos.X, 2) + pow(ankleRightPos.Z - footRightPos.Z, 2));
							float q = sqrt(pow(footLeftPos.X - footRightPos.X, 2) + pow(footLeftPos.Y - footRightPos.Y, 2) + pow(footLeftPos.Z - footRightPos.Z, 2));
;

							float r = sqrt(pow(spineBasePos.X - spineShoulderPos.X, 2) + pow(spineBasePos.Z - spineShoulderPos.Z, 2));
							float ss = sqrt(r + pow(spineShoulderPos.Y - spineBasePos.Y, 2));
							//float s = sqrt(pow(spineShoulderPos.Y - spineBasePos.Y, 2));
							float t = sqrt(pow(spineShoulderPos.X - spineBasePos.X, 2) + pow(spineShoulderPos.Y - spineBasePos.Y, 2) + pow(spineShoulderPos.Z - spineBasePos.Z, 2));

							float u = sqrt(pow(spineShoulderPos.X - ((kneeLeftPos.X + kneeRightPos.X) / 2), 2) + pow(spineShoulderPos.Y - ((kneeLeftPos.Y + kneeRightPos.Y) / 2), 2) + pow(spineShoulderPos.Z - ((kneeLeftPos.Z + kneeRightPos.Z) / 2), 2));
							float v = sqrt(pow(spineBasePos.X - ((kneeLeftPos.X + kneeRightPos.X) / 2), 2) + pow(spineBasePos.Y - ((kneeLeftPos.Y + kneeRightPos.Y) / 2), 2) + pow(spineBasePos.Z - ((kneeLeftPos.Z + kneeRightPos.Z) / 2), 2));
							//7 features from body joints
							float hei = headPos.Y - std::fmin(footLeftPos.Y, footRightPos.Y);
							float leftHipAngle = acos((pow(a, 2) + pow(b, 2) - pow(c, 2)) / (2 * a*b)) * 180 / PI;  
							float rightHipAngle = acos((pow(e, 2) + pow(d, 2) - pow(f, 2)) / (2 * e*d)) * 180 / PI; 
							float leftKneeAngle = acos((pow(a, 2) + pow(h, 2) - pow(g, 2)) / (2 * a*h)) * 180 / PI; 
							float rightKneeAngle = acos((pow(d, 2) + pow(j, 2) - pow(i, 2)) / (2 * d*j)) * 180 / PI; 
							float chestAngle;
							if ((pow(t, 2) + pow(ss, 2) - pow(r, 2)) / (2 * t*ss) > 0.995)
							{
							    chestAngle = 180;
							}
							else
							{
								chestAngle = 180 - (acos((pow(t, 2) + pow(ss, 2) - pow(r, 2)) / (2 * t*ss)) * 180 / PI);
							}
						    float chestKneeAngle = acos((pow(t, 2) + pow(v, 2) - pow(u, 2)) / (2 * t*v)) * 180 / PI;
							dataFile << hei << "," << leftHipAngle << "," << rightHipAngle << ",";
							dataFile << leftKneeAngle << "," << rightKneeAngle << "," << chestAngle << ",";
							dataFile << chestKneeAngle << ",";
							dataFile << std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count() << "\n";
							dataFile.close();

							////////////////////////////////////////////////
							//              Fall Detection               //
							///////////////////////////////////////////////
							if (Begin_State < Begin_threshold)
							{
								Begin_State++;
								//Depth_Computing_Sum = Depth_Computing_Sum + footLeftPos.Y + footRightPos.Y;
								Pos_Height_Computing_Sum = Pos_Height_Computing_Sum + hei;
								if (Begin_State == Begin_threshold)
								{
									//Floor_Depth = Depth_Computing_Sum / Begin_threshold / 2;
									Height_current = Pos_Height_Computing_Sum / Begin_threshold;
									if (Height_current > Height_Thres_Up_or_L) Pos_state = Upright;
									else Pos_state = Safe_Low;
									spineBaseheight_current = spineBasePos.Y;
									chestAngle_current = chestAngle;
									hei_current = hei;
									leftHipAngle_current = leftHipAngle;
									rightHipAngle_current = rightHipAngle;
									leftKneeAngle_current = leftKneeAngle;
									rightKneeAngle_current = rightKneeAngle;
									chestKneeAngle_current = chestKneeAngle;
								}
							}
							else
							{
								float fall_speed = spineBasePos.Y - spineBaseheight_current;
								float lean_speed = chestAngle - chestAngle_current;
								float change_speed = abs(chestAngle - chestAngle_current);
								change_speed = change_speed + abs(hei - hei_current);
								change_speed = change_speed + abs(leftHipAngle - leftHipAngle_current);
								change_speed = change_speed + abs(rightHipAngle - rightHipAngle_current);
								change_speed = change_speed + abs(leftKneeAngle - leftKneeAngle_current);
								change_speed = change_speed + abs(rightKneeAngle - rightKneeAngle_current);
								change_speed = change_speed + abs(chestKneeAngle - chestKneeAngle_current);

								spineBaseheight_current = spineBasePos.Y;
								chestAngle_current = chestAngle;
								hei_current = hei;
								leftHipAngle_current = leftHipAngle;
								rightHipAngle_current = rightHipAngle;
								leftKneeAngle_current = leftKneeAngle;
								rightKneeAngle_current = rightKneeAngle;
								chestKneeAngle_current = chestKneeAngle;
								std::ofstream dataFile;
								dataFile.open("speed_fast.csv", std::ofstream::out | std::ofstream::app);
								dataFile << fall_speed << "," << lean_speed << "\n";
								dataFile.close();
								wchar_t text[20];
								switch (Pos_state){
								case Upright:
									lstrcpyW(text, L"Upright£¡");
									if ((abs(fall_speed) > Fall_speed_Thres) || (lean_speed > Lean_speed_Thres))
										Pos_state = Leaning_rapidly;
									else
									{
										if (hei < Height_Thres_Up_or_L)
											Pos_state = Safe_Low;
										else
											Doubt_Count = 0;
									}
									break;
								case Leaning_rapidly:
									lstrcpyW(text, L"Leaning_rapidly£¡");
									if (hei < Height_Thres_Up_or_L)
										Pos_state = Low;
									else
									{
										if ((abs(fall_speed) > Fall_speed_Thres) && (lean_speed > Lean_speed_Thres))
										{ 
											Pos_state = Leaning_rapidly;
											Doubt_Count = 0;
										}
										else
										{
											Doubt_Count++;
											if (Doubt_Count == Doubt_Count_Thres_LR_to_LS)
											{
												Pos_state = Leaning_safely;
												Doubt_Count = 0;
											}
										}
									}
									break;
								case Leaning_safely:
									lstrcpyW(text, L"Leaning_safely£¡");
									if ((abs(fall_speed) > Fall_speed_Thres) || (lean_speed > Lean_speed_Thres))
									{
										Doubt_Count++;
										if (Doubt_Count == Doubt_Count_Thres_LS_to_LR)
										{
											Pos_state = Leaning_rapidly;
											Doubt_Count = 0;
										}
									}
									else
									{
										if (hei < Height_Thres_Up_or_L)
										{
											Pos_state = Safe_Low;
											Doubt_Count = 0;
										}
										else
										{
											Doubt_Count = 0;
										}
									}
									break;
								case Low:
									lstrcpyW(text, L"Low£¡");
									if (hei > Height_Thres_Up_or_L) Pos_state = Upright;
									else
									{
										if(change_speed < Change_speed_Thres)
										{
											Doubt_Count++;
											if (Doubt_Count == FALL_DETECTION_THRES)
											{
												Pos_state = Fall;
												Doubt_Count = 0;
											}
										}
										else
										{
											if (Doubt_Count != 0) Doubt_Count = Doubt_Count - 1;
										}
									}
									break;
								case Safe_Low:
									lstrcpyW(text, L"Safe_Low£¡");
									if (hei > Height_Thres_Up_or_L) Pos_state = Upright;
									break;
								case Fall:
									lstrcpyW(text, L"Fall£¡");
									Pos_state = Fall;
									break;
								default: {}
								}
								m_pRenderTarget->DrawText(
									text,           // Text to render
									wcslen(text),       // Text length
									g_pTextFormat,     // Text format
									textLayoutRect,    // The region of the window where the text will be rendered
									m_pBrushText      // The brush used to draw the text
								);
							}
							
							for (int j = 0; j < _countof(joints); ++j)
							{
								jointPoints[j] = BodyToScreen(joints[j].Position, width, height);
							}

							DrawBody(joints, jointPoints);

							DrawHand(leftHandState, jointPoints[JointType_HandLeft]);
							DrawHand(rightHandState, jointPoints[JointType_HandRight]);
						}
					}
				}
			}

			hr = m_pRenderTarget->EndDraw();

			// Device lost, need to recreate the render target
			// We'll dispose it now and retry drawing
			if (D2DERR_RECREATE_TARGET == hr)
			{
				hr = S_OK;
				DiscardDirect2DResources();
			}
		}

		if (!m_nStartTime)
		{
			m_nStartTime = nTime;
		}

		double fps = 0.0;

		LARGE_INTEGER qpcNow = { 0 };
		if (m_fFreq)
		{
			if (QueryPerformanceCounter(&qpcNow))
			{
				if (m_nLastCounter)
				{
					m_nFramesSinceUpdate++;
					fps = m_fFreq * m_nFramesSinceUpdate / double(qpcNow.QuadPart - m_nLastCounter);
				}
			}
		}

		WCHAR szStatusMessage[64];
		StringCchPrintf(szStatusMessage, _countof(szStatusMessage), L" FPS = %0.2f    Time = %I64d", fps, (nTime - m_nStartTime));

		if (SetStatusMessage(szStatusMessage, 1000, false))
		{
			m_nLastCounter = qpcNow.QuadPart;
			m_nFramesSinceUpdate = 0;
		}
	}
}

/// <summary>
/// Set the status bar message
/// </summary>
/// <param name="szMessage">message to display</param>
/// <param name="showTimeMsec">time in milliseconds to ignore future status messages</param>
/// <param name="bForce">force status update</param>
bool CBodyBasics::SetStatusMessage(_In_z_ WCHAR* szMessage, DWORD nShowTimeMsec, bool bForce)
{
	INT64 now = GetTickCount64();

	if (m_hWnd && (bForce || (m_nNextStatusTime <= now)))
	{
		SetDlgItemText(m_hWnd, IDC_STATUS, szMessage);
		m_nNextStatusTime = now + nShowTimeMsec;

		return true;
	}

	return false;
}

/// <summary>
/// Ensure necessary Direct2d resources are created
/// </summary>
/// <returns>S_OK if successful, otherwise an error code</returns>
HRESULT CBodyBasics::EnsureDirect2DResources()
{
	HRESULT hr = S_OK;

	if (m_pD2DFactory && !m_pRenderTarget)
	{
		RECT rc;
		GetWindowRect(GetDlgItem(m_hWnd, IDC_VIDEOVIEW), &rc);

		int width = rc.right - rc.left;
		int height = rc.bottom - rc.top;
		D2D1_SIZE_U size = D2D1::SizeU(width, height);
		D2D1_RENDER_TARGET_PROPERTIES rtProps = D2D1::RenderTargetProperties();
		rtProps.pixelFormat = D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_IGNORE);
		rtProps.usage = D2D1_RENDER_TARGET_USAGE_GDI_COMPATIBLE;

		// Create a Hwnd render target, in order to render to the window set in initialize
		hr = m_pD2DFactory->CreateHwndRenderTarget(
			rtProps,
			D2D1::HwndRenderTargetProperties(GetDlgItem(m_hWnd, IDC_VIDEOVIEW), size),
			&m_pRenderTarget
		);

		if (FAILED(hr))
		{
			SetStatusMessage(L"Couldn't create Direct2D render target!", 10000, true);
			return hr;
		}

		// light green
		m_pRenderTarget->CreateSolidColorBrush(D2D1::ColorF(0.27f, 0.75f, 0.27f), &m_pBrushJointTracked);

		m_pRenderTarget->CreateSolidColorBrush(D2D1::ColorF(D2D1::ColorF::Yellow, 1.0f), &m_pBrushJointInferred);
		m_pRenderTarget->CreateSolidColorBrush(D2D1::ColorF(D2D1::ColorF::Green, 1.0f), &m_pBrushBoneTracked);
		m_pRenderTarget->CreateSolidColorBrush(D2D1::ColorF(D2D1::ColorF::Gray, 1.0f), &m_pBrushBoneInferred);

		m_pRenderTarget->CreateSolidColorBrush(D2D1::ColorF(D2D1::ColorF::Red, 0.5f), &m_pBrushHandClosed);
		m_pRenderTarget->CreateSolidColorBrush(D2D1::ColorF(D2D1::ColorF::Green, 0.5f), &m_pBrushHandOpen);
		m_pRenderTarget->CreateSolidColorBrush(D2D1::ColorF(D2D1::ColorF::Blue, 0.5f), &m_pBrushHandLasso);
	}

	return hr;
}

/// <summary>
/// Dispose Direct2d resources 
/// </summary>
void CBodyBasics::DiscardDirect2DResources()
{
	SafeRelease(m_pRenderTarget);

	SafeRelease(m_pBrushJointTracked);
	SafeRelease(m_pBrushJointInferred);
	SafeRelease(m_pBrushBoneTracked);
	SafeRelease(m_pBrushBoneInferred);

	SafeRelease(m_pBrushHandClosed);
	SafeRelease(m_pBrushHandOpen);
	SafeRelease(m_pBrushHandLasso);
}

/// <summary>
/// Converts a body point to screen space
/// </summary>
/// <param name="bodyPoint">body point to tranform</param>
/// <param name="width">width (in pixels) of output buffer</param>
/// <param name="height">height (in pixels) of output buffer</param>
/// <returns>point in screen-space</returns>
D2D1_POINT_2F CBodyBasics::BodyToScreen(const CameraSpacePoint& bodyPoint, int width, int height)
{
	// Calculate the body's position on the screen
	DepthSpacePoint depthPoint = { 0 };
	m_pCoordinateMapper->MapCameraPointToDepthSpace(bodyPoint, &depthPoint);

	float screenPointX = static_cast<float>(depthPoint.X * width) / cDepthWidth;
	float screenPointY = static_cast<float>(depthPoint.Y * height) / cDepthHeight;

	return D2D1::Point2F(screenPointX, screenPointY);
}

/// <summary>
/// Draws a body 
/// </summary>
/// <param name="pJoints">joint data</param>
/// <param name="pJointPoints">joint positions converted to screen space</param>
void CBodyBasics::DrawBody(const Joint* pJoints, const D2D1_POINT_2F* pJointPoints)
{
	// Draw the bones

	// Torso
	DrawBone(pJoints, pJointPoints, JointType_Head, JointType_Neck);
	DrawBone(pJoints, pJointPoints, JointType_Neck, JointType_SpineShoulder);
	DrawBone(pJoints, pJointPoints, JointType_SpineShoulder, JointType_SpineMid);
	DrawBone(pJoints, pJointPoints, JointType_SpineMid, JointType_SpineBase);
	DrawBone(pJoints, pJointPoints, JointType_SpineShoulder, JointType_ShoulderRight);
	DrawBone(pJoints, pJointPoints, JointType_SpineShoulder, JointType_ShoulderLeft);
	DrawBone(pJoints, pJointPoints, JointType_SpineBase, JointType_HipRight);
	DrawBone(pJoints, pJointPoints, JointType_SpineBase, JointType_HipLeft);

	// Right Arm    
	DrawBone(pJoints, pJointPoints, JointType_ShoulderRight, JointType_ElbowRight);
	DrawBone(pJoints, pJointPoints, JointType_ElbowRight, JointType_WristRight);
	DrawBone(pJoints, pJointPoints, JointType_WristRight, JointType_HandRight);
	DrawBone(pJoints, pJointPoints, JointType_HandRight, JointType_HandTipRight);
	DrawBone(pJoints, pJointPoints, JointType_WristRight, JointType_ThumbRight);

	// Left Arm
	DrawBone(pJoints, pJointPoints, JointType_ShoulderLeft, JointType_ElbowLeft);
	DrawBone(pJoints, pJointPoints, JointType_ElbowLeft, JointType_WristLeft);
	DrawBone(pJoints, pJointPoints, JointType_WristLeft, JointType_HandLeft);
	DrawBone(pJoints, pJointPoints, JointType_HandLeft, JointType_HandTipLeft);
	DrawBone(pJoints, pJointPoints, JointType_WristLeft, JointType_ThumbLeft);

	// Right Leg
	DrawBone(pJoints, pJointPoints, JointType_HipRight, JointType_KneeRight);
	DrawBone(pJoints, pJointPoints, JointType_KneeRight, JointType_AnkleRight);
	DrawBone(pJoints, pJointPoints, JointType_AnkleRight, JointType_FootRight);

	// Left Leg
	DrawBone(pJoints, pJointPoints, JointType_HipLeft, JointType_KneeLeft);
	DrawBone(pJoints, pJointPoints, JointType_KneeLeft, JointType_AnkleLeft);
	DrawBone(pJoints, pJointPoints, JointType_AnkleLeft, JointType_FootLeft);

	// Draw the joints
	for (int i = 0; i < JointType_Count; ++i)
	{
		D2D1_ELLIPSE ellipse = D2D1::Ellipse(pJointPoints[i], c_JointThickness, c_JointThickness);

		if (pJoints[i].TrackingState == TrackingState_Inferred)
		{
			m_pRenderTarget->FillEllipse(ellipse, m_pBrushJointInferred);
		}
		else if (pJoints[i].TrackingState == TrackingState_Tracked)
		{
			m_pRenderTarget->FillEllipse(ellipse, m_pBrushJointTracked);
		}
	}
}

/// <summary>
/// Draws one bone of a body (joint to joint)
/// </summary>
/// <param name="pJoints">joint data</param>
/// <param name="pJointPoints">joint positions converted to screen space</param>
/// <param name="pJointPoints">joint positions converted to screen space</param>
/// <param name="joint0">one joint of the bone to draw</param>
/// <param name="joint1">other joint of the bone to draw</param>
void CBodyBasics::DrawBone(const Joint* pJoints, const D2D1_POINT_2F* pJointPoints, JointType joint0, JointType joint1)
{
	TrackingState joint0State = pJoints[joint0].TrackingState;
	TrackingState joint1State = pJoints[joint1].TrackingState;

	// If we can't find either of these joints, exit
	if ((joint0State == TrackingState_NotTracked) || (joint1State == TrackingState_NotTracked))
	{
		return;
	}

	// Don't draw if both points are inferred
	if ((joint0State == TrackingState_Inferred) && (joint1State == TrackingState_Inferred))
	{
		return;
	}

	// We assume all drawn bones are inferred unless BOTH joints are tracked
	if ((joint0State == TrackingState_Tracked) && (joint1State == TrackingState_Tracked))
	{
		m_pRenderTarget->DrawLine(pJointPoints[joint0], pJointPoints[joint1], m_pBrushBoneTracked, c_TrackedBoneThickness);
	}
	else
	{
		m_pRenderTarget->DrawLine(pJointPoints[joint0], pJointPoints[joint1], m_pBrushBoneInferred, c_InferredBoneThickness);
	}
}

/// <summary>
/// Draws a hand symbol if the hand is tracked: red circle = closed, green circle = opened; blue circle = lasso
/// </summary>
/// <param name="handState">state of the hand</param>
/// <param name="handPosition">position of the hand</param>
void CBodyBasics::DrawHand(HandState handState, const D2D1_POINT_2F& handPosition)
{
	D2D1_ELLIPSE ellipse = D2D1::Ellipse(handPosition, c_HandSize, c_HandSize);

	switch (handState)
	{
	case HandState_Closed:
		m_pRenderTarget->FillEllipse(ellipse, m_pBrushHandClosed);
		break;

	case HandState_Open:
		m_pRenderTarget->FillEllipse(ellipse, m_pBrushHandOpen);
		break;

	case HandState_Lasso:
		m_pRenderTarget->FillEllipse(ellipse, m_pBrushHandLasso);
		break;
	}
}

