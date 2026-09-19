#pragma once
#include "ui/control_settings.h"
#include "ui/palette.h"
inline ColorRole controlColorRole(Style::ControlPaletteRole role) {
  switch (role) {
  case Style::ControlPaletteRole::Surface: return ColorRole::Surface;
  case Style::ControlPaletteRole::SurfaceVariant: return ColorRole::SurfaceVariant;
  case Style::ControlPaletteRole::Primary: return ColorRole::Primary;
  case Style::ControlPaletteRole::Secondary: return ColorRole::Secondary;
  case Style::ControlPaletteRole::OnSurface: return ColorRole::OnSurface;
  case Style::ControlPaletteRole::OnPrimary: return ColorRole::OnPrimary;
  }
  return ColorRole::Surface;
}
inline ColorRole controlForegroundRole(Style::ControlPaletteRole role) {
  switch (role) {
  case Style::ControlPaletteRole::Primary: return ColorRole::OnPrimary;
  case Style::ControlPaletteRole::Secondary: return ColorRole::OnSecondary;
  case Style::ControlPaletteRole::OnSurface: return ColorRole::Surface;
  case Style::ControlPaletteRole::OnPrimary: return ColorRole::Primary;
  case Style::ControlPaletteRole::Surface:
  case Style::ControlPaletteRole::SurfaceVariant: return ColorRole::OnSurface;
  }
  return ColorRole::OnSurface;
}
