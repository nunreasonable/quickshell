#pragma once

#include <memory>

#include <qt_windows.h>

#include <d3d11.h>

#include <qrect.h>
#include <qstring.h>
#include <qtclasshelpermacros.h>

namespace qs::windows::recorder {

struct RegionCaptureState;

///! Windows.Graphics.Capture of every monitor under a desktop region, composed into one texture.
/// Each monitor gets its own session on a free threaded frame pool; frames are copied straight
/// from the thread pool callback into their place in `composite()`, so the encoder side reads
/// whatever is current at each tick (WGC only delivers frames when something changed, and a
/// static screen would otherwise stall the video). Parts no monitor covers stay black.
///
/// Unlike capture.cpp's sessions this one works in BGRA, the layout Media Foundation's encoders
/// take (RGB32/ARGB32), and on the recorder's own device so frames never cross devices.
/// Create, start and stop on an MTA thread.
class RegionCapture {
public:
	RegionCapture() = default;
	~RegionCapture();
	Q_DISABLE_COPY_MOVE(RegionCapture);

	// `region` is in physical pixels, virtual desktop coordinates. `device` must be
	// multithread protected: callbacks use its immediate context from other threads.
	bool start(ID3D11Device* device, const QRect& region, bool cursor, QString* error);
	void stop();

	[[nodiscard]] ID3D11Texture2D* composite() const;
	// Every monitor delivered at least one frame.
	[[nodiscard]] bool primed() const;
	// A monitor's capture item closed (monitor unplugged, display mode reset).
	[[nodiscard]] bool lost() const;

private:
	std::shared_ptr<RegionCaptureState> state;
};

} // namespace qs::windows::recorder
