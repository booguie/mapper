/*
 *    Copyright 2012-2015, 2019 Kai Pastor
 *
 *    This file is part of OpenOrienteering.
 *
 *    OpenOrienteering is free software: you can redistribute it and/or modify
 *    it under the terms of the GNU General Public License as published by
 *    the Free Software Foundation, either version 3 of the License, or
 *    (at your option) any later version.
 *
 *    OpenOrienteering is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU General Public License for more details.
 *
 *    You should have received a copy of the GNU General Public License
 *    along with OpenOrienteering.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "georeferencing_t.h"

#include <cmath>
#include <cstddef>

#include <QtMath>
#include <QtTest>
#include <QByteArray>
#include <QLineF>
#include <QPoint>
#include <QPointF>

#include <geodesic.h>
#ifndef ACCEPT_USE_OF_DEPRECATED_PROJ_API_H
#  include <proj.h>
#endif

#ifdef MAPPER_USE_GDAL
#  include <gdal.h>
#  include <ogr_api.h>
#  include <ogr_srs_api.h>
// IWYU pragma: no_include <gdal_version.h>
#endif

#include "core/crs_template.h"
#include "core/georeferencing.h"
#include "core/latlon.h"
#include "core/map_coord.h"
#include "fileformats/xml_file_format.h"


using namespace OpenOrienteering;


#ifndef MAPPER_COMMON_LIB
// mock stuff to satisfy link-time dependencies
int XMLFileFormat::active_version = 6;
#endif


namespace
{
	// clazy:excludeall=non-pod-global-static
	auto const epsg3857_spec = QStringLiteral("+init=epsg:3857");
	auto const epsg5514_spec = QStringLiteral("+init=epsg:5514");
	auto const gk2_spec   = QStringLiteral("+proj=tmerc +lat_0=0 +lon_0=6 +k=1.000000 +x_0=2500000 +y_0=0 +ellps=bessel +datum=potsdam +units=m +no_defs");
	auto const gk3_spec   = QStringLiteral("+proj=tmerc +lat_0=0 +lon_0=9 +k=1.000000 +x_0=3500000 +y_0=0 +ellps=bessel +datum=potsdam +units=m +no_defs");
	auto const utm32_spec = QStringLiteral("+proj=utm +zone=32 +datum=WGS84");
	
	
	/**
	 * Returns the radian value of a value given in degree or degree/minutes/seconds.
	 */
	double degFromDMS(double d, double m=0.0, double s=0.0)
	{
		return d + m/60.0 + s/3600.0;
	}
	
	
	/**
	 * Returns the geodetic distance between the given geographic coordinates.
	 * 
	 * This function uses the WGS84 ellipsoid.
	 */
	double geodeticDistance(const LatLon& first, const LatLon& second)
	{
		auto distance = 0.0;
		auto g = geod_geodesic{};
		geod_init(&g, 6378137, 1/298.257223563);  // WGS84 ellipsoid
		geod_inverse(&g, first.latitude(), first.longitude(), second.latitude(), second.longitude(),
		             &distance, nullptr, nullptr);
		return distance;
	}
	
	/**
	 * Returns the nominal east-west scale factor for "Web Mercator".
	 */
	double epsg3857ScaleX(double latitude)
	{
		constexpr auto e = 0.081819191; // WGS84 eccentricity
		auto phi = qDegreesToRadians(latitude);
		return pow(1.0 - e * e * sin(phi) * sin(phi), 0.5) / cos(phi);
	}
	
	/**
	 * Returns the nominal north-south scale factor for "Web Mercator".
	 */
	double epsg3857ScaleY(double latitude)
	{
		constexpr auto e = 0.081819191; // WGS84 eccentricity
		auto phi = qDegreesToRadians(latitude);
		return pow(1.0 - e * e * sin(phi) * sin(phi), 1.5) / (1.0 - e * e) / cos(phi);
	}
	
}  // namespace


void GeoreferencingTest::initTestCase()
{
	XMLFileFormat::active_version = 6;
}


void GeoreferencingTest::testEmptyProjectedCRS()
{
	Georeferencing new_georef;
	QVERIFY(new_georef.isValid());
	QVERIFY(new_georef.isLocal());
	QCOMPARE(new_georef.getState(), Georeferencing::Local);
	QCOMPARE(new_georef.getScaleDenominator(), 1000u);
	QCOMPARE(new_georef.getCombinedScaleFactor(), 1.0);
	QCOMPARE(new_georef.getAuxiliaryScaleFactor(), 1.0);
	QCOMPARE(new_georef.getDeclination(), 0.0);
	QCOMPARE(new_georef.getGrivation(), 0.0);
	QCOMPARE(new_georef.getGrivationError(), 0.0);
	QCOMPARE(new_georef.getConvergence(), 0.0);
	QCOMPARE(new_georef.getMapRefPoint(), MapCoord(0, 0));
	QCOMPARE(new_georef.getProjectedRefPoint(), QPointF(0.0, 0.0));
}

void GeoreferencingTest::testGridScaleFactor_data()
{
	QTest::addColumn<QString>("spec");
	QTest::addColumn<double>("lat");
	QTest::addColumn<double>("lon");
	QTest::addColumn<double>("scale_x");
	QTest::addColumn<double>("scale_y");
	
	QTest::newRow("UTM 32 central meridian")    << utm32_spec     << 50.0 << 9.0  << 0.9996 << 0.9996;
	QTest::newRow("UTM 32 180 km west of c.m.") << utm32_spec     << 50.0 << 6.48 << 1.0    << 1.0;
	QTest::newRow("EPSG 3857")                  << epsg3857_spec  << 50.0 << 6.48 << epsg3857ScaleX(50.0) << epsg3857ScaleY(50.0);
}

void GeoreferencingTest::testGridScaleFactor()
{
	QFETCH(QString, spec);
	QFETCH(double, lat);
	QFETCH(double, lon);
	QFETCH(double, scale_x);
	QFETCH(double, scale_y);
	
	Georeferencing georef;
	georef.setProjectedCRS(QString::fromLatin1(QTest::currentDataTag()), spec);
	QVERIFY2(georef.isValid(), georef.getErrorText().toLatin1());
	
	auto const latlon = LatLon{lat, lon};
	georef.setGeographicRefPoint(latlon);
	QVERIFY2(georef.isValid(), georef.getErrorText().toLatin1());
	
	// Verify scale_x
	auto const east = georef.getProjectedRefPoint() + QPointF{500.0, 0.0};
	auto const west = georef.getProjectedRefPoint() - QPointF{500.0, 0.0};
	auto const grid_distance_x = QLineF{east, west}.length();
	auto const geod_distance_x = geodeticDistance(georef.toGeographicCoords(west), georef.toGeographicCoords(east));
	if (std::fabs(grid_distance_x - geod_distance_x * scale_x) >= 0.001)
	{
		QVERIFY(geod_distance_x > 0.0);
		QCOMPARE(grid_distance_x / geod_distance_x, scale_x);
	}
	
	// Verify scale_y
	auto const north = georef.getProjectedRefPoint() - QPointF{0.0, 500.0};
	auto const south = georef.getProjectedRefPoint() + QPointF{0.0, 500.0};
	auto const grid_distance_y = QLineF{north, south}.length();
	auto const geod_distance_y = geodeticDistance(georef.toGeographicCoords(north), georef.toGeographicCoords(south));
	if (std::fabs(grid_distance_y - geod_distance_y * scale_y) >= 0.001)
	{
		QVERIFY(geod_distance_y > 0.0);
		QCOMPARE(grid_distance_y / geod_distance_y, scale_y);
	}
	
	// Apply the average scale factor to the georeferencing, and
	// compare geodetic distance against ground distance, based on length in map.
	auto const sw = georef.getProjectedRefPoint() - QPointF{100.0, 100.0};
	auto const ne = georef.getProjectedRefPoint() + QPointF{100.0, 100.0};
	auto const geod_distance = geodeticDistance(georef.toGeographicCoords(sw), georef.toGeographicCoords(ne));
	
	// Try the georeferencing's automatic scale factor.
	auto map_distance = QLineF{georef.toMapCoordF(sw), georef.toMapCoordF(ne)}.length();
	auto ground_distance = map_distance * georef.getScaleDenominator() / 1000;
	if (std::fabs(geod_distance - ground_distance) >= 0.001)
	{
		QCOMPARE(geod_distance, ground_distance);
	}
	
	georef.setCombinedScaleFactor((scale_x + scale_y) / 2);
	map_distance = QLineF{georef.toMapCoordF(sw), georef.toMapCoordF(ne)}.length();
	ground_distance = map_distance * georef.getScaleDenominator() / 1000;
	if (std::fabs(geod_distance - ground_distance) >= 0.001)
	{
		QCOMPARE(geod_distance, ground_distance);
	}
	
	// And again, with significant declination
	georef.setDeclination(20.0);
	map_distance = QLineF{georef.toMapCoordF(sw), georef.toMapCoordF(ne)}.length();
	ground_distance = map_distance * georef.getScaleDenominator() / 1000;
	if (std::fabs(geod_distance - ground_distance) >= 0.001)
	{
		QCOMPARE(geod_distance, ground_distance);
	}
	
	// Try setting the georeferencing's auxiliary scale factor.
	const double elevation_scale_factor = 1.1;
	georef.setAuxiliaryScaleFactor(elevation_scale_factor);
	map_distance = QLineF{georef.toMapCoordF(sw), georef.toMapCoordF(ne)}.length();
	ground_distance = map_distance * georef.getScaleDenominator() / 1000;
	if (std::fabs(geod_distance - elevation_scale_factor * ground_distance) >= 0.001)
	{
		QCOMPARE(geod_distance, elevation_scale_factor * ground_distance);
	}

	// Finally, see that the auxiliary scale factor is preserved when
	// the CRS changes.
	georef.setProjectedCRS(QString::fromLatin1(QTest::currentDataTag()), utm32_spec);
	QVERIFY2(georef.isValid(), georef.getErrorText().toLatin1());
	QCOMPARE(georef.getAuxiliaryScaleFactor(), elevation_scale_factor);
	map_distance = QLineF{georef.toMapCoordF(sw), georef.toMapCoordF(ne)}.length();
	ground_distance = map_distance * georef.getScaleDenominator() / 1000;
	if (std::fabs(geod_distance - elevation_scale_factor * ground_distance) >= 0.001)
	{
		QCOMPARE(geod_distance, elevation_scale_factor * ground_distance);
	}
}

void GeoreferencingTest::testCRS_data()
{
	QTest::addColumn<QString>("id");
	QTest::addColumn<QString>("spec");
	
	QTest::newRow("EPSG:4326") << QStringLiteral("EPSG:4326") << QStringLiteral("+init=epsg:4326");
	QTest::newRow("UTM")       << QStringLiteral("UTM")       << utm32_spec;
}

void GeoreferencingTest::testCRS()
{
	QFETCH(QString, id);
	QFETCH(QString, spec);
	Georeferencing georef;
	QVERIFY2(georef.setProjectedCRS(id, spec), georef.getErrorText().toLatin1());
}


void GeoreferencingTest::testCRSTemplates()
{
	auto epsg_template = CRSTemplateRegistry().find(QStringLiteral("EPSG"));
	QCOMPARE(epsg_template->parameters().size(), static_cast<std::size_t>(1));
	
	QCOMPARE(epsg_template->coordinatesName(), QStringLiteral("EPSG @code@ coordinates"));
	QCOMPARE(epsg_template->coordinatesName({ QStringLiteral("4326") }), QStringLiteral("EPSG 4326 coordinates"));
	
	Georeferencing georef;
	georef.setProjectedCRS(QStringLiteral("EPSG"), epsg_template->specificationTemplate().arg(QStringLiteral("5514")), { QStringLiteral("5514") });
	QVERIFY(georef.isValid());
	QCOMPARE(georef.getProjectedCoordinatesName(), QStringLiteral("EPSG 5514 coordinates"));
}


void GeoreferencingTest::testProjection_data()
{
	QTest::addColumn<QString>("proj");
	QTest::addColumn<double>("easting");
	QTest::addColumn<double>("northing");
	QTest::addColumn<double>("latitude");
	QTest::addColumn<double>("longitude");
	
	// Record name                               CRS spec      Easting      Northing     Latitude (radian)           Longitude (radian)
	// Selected from http://www.lvermgeo.rlp.de/index.php?id=5809,
	// now https://lvermgeo.rlp.de/de/service/gut-zu-wissen/arbeitet-ihr-gps-empfaenger-korrekt/
	QTest::newRow("LVermGeo RLP Koblenz UTM") << utm32_spec <<  398125.0 << 5579523.0 << degFromDMS(50, 21, 32.2) << degFromDMS( 7, 34, 4.0);
	QTest::newRow("LVermGeo RLP Koblenz GK3") << gk3_spec   << 3398159.0 << 5581315.0 << degFromDMS(50, 21, 32.2) << degFromDMS( 7, 34, 4.0);
	QTest::newRow("LVermGeo RLP Pruem UTM")   << utm32_spec <<  316464.0 << 5565150.0 << degFromDMS(50, 12, 36.1) << degFromDMS( 6, 25, 39.6);
	QTest::newRow("LVermGeo RLP Pruem GK2")   << gk2_spec   << 2530573.0 << 5563858.0 << degFromDMS(50, 12, 36.1) << degFromDMS( 6, 25, 39.6);
	QTest::newRow("LVermGeo RLP Landau UTM")  << utm32_spec <<  436705.0 << 5450182.0 << degFromDMS(49, 12,  4.2) << degFromDMS( 8,  7, 52.0);
	QTest::newRow("LVermGeo RLP Landau GK3")  << gk3_spec   << 3436755.0 << 5451923.0 << degFromDMS(49, 12,  4.2) << degFromDMS( 8,  7, 52.0);
	
	// Selected from http://geoportal.cuzk.cz/geoprohlizec/?lng=EN, source Bodová pole, layer Bod ZPBP určený v ETRS
	// http://dataz.cuzk.cz/gu.php?1=25&2=08&3=024&4=a&stamp=7oexTQLNPE8ri5SXY04ARS7vIMnJu3N2
	QTest::newRow("EPSG 5514 ČÚZK Dolní Temenice") << epsg5514_spec   << -563714.79 << -1076943.54 << degFromDMS(49, 58, 37.5577) << degFromDMS(16, 57, 35.5493);
	
	// Swiss CH1903+/LV95
	auto epsg2056_spec = QStringLiteral("+init=epsg:2056");
	// Projection centre, according to https://www.epsg-registry.org/
	QTest::newRow("EPSG 2056 Bern") << epsg2056_spec << 2600000.0 << 1200000.0 << degFromDMS(46, 57, 03.898) << degFromDMS(7, 26, 19.077);
	// Issue GH-1325
	QTest::newRow("EPSG 2056 GH-1325") << epsg2056_spec << 2643092.73 << 1150008.01 << 46.5 << 8.0;
	
	// EPSG:27700, OSGB 36
	auto epsg27700_spec = QStringLiteral("+init=epsg:27700");
	QTest::newRow("EPSG 27700 NY 06071 11978") << epsg27700_spec << 306071.0  << 511978.0 << 54.494403  << -3.4517026;
	QTest::newRow("EPSG 27700 Lake District")  << epsg27700_spec << 306074.66 << 511974.0 << 54.4943673 << -3.4516448;
}


void GeoreferencingTest::testProjection()
{
	const double max_dist_error = 2.2; // meter
	const double max_angl_error = 0.00005; // degrees
	
	QFETCH(QString, proj);
	
	Georeferencing georef;
	QVERIFY2(georef.setProjectedCRS(proj, proj), proj.toLatin1());
	QCOMPARE(georef.getErrorText(), QString{});
	
	QFETCH(double, easting);
	QFETCH(double, northing);
	QFETCH(double, latitude);
	QFETCH(double, longitude);
	
	// geographic to projected
	LatLon lat_lon(latitude, longitude);
	bool ok;
	QPointF proj_coord = georef.toProjectedCoords(lat_lon, &ok);
	QVERIFY(ok);
	
	if (std::fabs(proj_coord.x() - easting) > max_dist_error)
		QCOMPARE(QString::number(proj_coord.x(), 'f'), QString::number(easting, 'f'));
	if (std::fabs(proj_coord.y() - northing) > max_dist_error)
		QCOMPARE(QString::number(proj_coord.y(), 'f'), QString::number(northing, 'f'));
	
	// projected to geographic
	proj_coord = QPointF(easting, northing);
	lat_lon = georef.toGeographicCoords(proj_coord, &ok);
	QVERIFY(ok);
	if (std::fabs(lat_lon.latitude() - latitude) > max_angl_error)
		QCOMPARE(QString::number(lat_lon.latitude(), 'f'), QString::number(latitude, 'f'));
	if (std::fabs(lat_lon.longitude() - longitude) > (max_angl_error * std::cos(qDegreesToRadians(latitude))))
		QCOMPARE(QString::number(lat_lon.longitude(), 'f'), QString::number(longitude, 'f'));
	
#ifdef MAPPER_USE_GDAL
	// Cf. OgrFileExport::setupGeoreferencing
	auto* map_srs = OSRNewSpatialReference(nullptr);
	OSRSetProjCS(map_srs, "Projected map SRS");
	OSRSetWellKnownGeogCS(map_srs, "WGS84");
	auto spec = QByteArray(georef.getProjectedCRSSpec().toLatin1());
	QCOMPARE(OSRImportFromProj4(map_srs, spec), OGRERR_NONE);
	auto* geo_srs = OSRNewSpatialReference(nullptr);
	OSRSetWellKnownGeogCS(geo_srs, "WGS84");
#if GDAL_VERSION_MAJOR >= 3
	OSRSetAxisMappingStrategy(geo_srs, OAMS_TRADITIONAL_GIS_ORDER);
#endif
	
	// geographic to projected
	auto* transformation = OCTNewCoordinateTransformation(geo_srs, map_srs);
	auto* pt = OGR_G_CreateGeometry(wkbPoint);
	OGR_G_SetPoint_2D(pt, 0, longitude, latitude);
	QCOMPARE(OGR_G_Transform(pt, transformation), OGRERR_NONE);
	if (std::fabs(OGR_G_GetX(pt, 0) - easting) > max_dist_error)
		QCOMPARE(QString::number(OGR_G_GetX(pt, 0), 'f'), QString::number(easting, 'f'));
	if (std::fabs(OGR_G_GetY(pt, 0) - northing) > max_dist_error)
		QCOMPARE(QString::number(OGR_G_GetY(pt, 0), 'f'), QString::number(northing, 'f'));
	OGR_G_DestroyGeometry(pt);
	OCTDestroyCoordinateTransformation(transformation);
	
	// projected to geographic
	transformation = OCTNewCoordinateTransformation(map_srs, geo_srs);
	// Cf. OgrFileExport::addPointsToLayer
	pt = OGR_G_CreateGeometry(wkbPoint);
	OGR_G_SetPoint_2D(pt, 0, easting, northing);
	QCOMPARE(OGR_G_Transform(pt, transformation), OGRERR_NONE);
	if (std::fabs(OGR_G_GetY(pt, 0) - latitude) > max_angl_error)
		QCOMPARE(QString::number(OGR_G_GetY(pt, 0), 'f'), QString::number(latitude, 'f'));
	if (std::fabs(OGR_G_GetX(pt, 0) - longitude) > (max_angl_error * std::cos(qDegreesToRadians(latitude))))
		QCOMPARE(QString::number(OGR_G_GetX(pt, 0), 'f'), QString::number(longitude, 'f'));
	OGR_G_DestroyGeometry(pt);
	OCTDestroyCoordinateTransformation(transformation);
	
	OSRDestroySpatialReference(geo_srs);
	OSRDestroySpatialReference(map_srs);
#endif  // MAPPPER_USE_GDAL
}



#ifndef ACCEPT_USE_OF_DEPRECATED_PROJ_API_H

namespace {
	bool finder_called;

	extern "C"
	const char* projFinderTestFakeCRS(PJ_CONTEXT* /*ctx*/, const char* name, void* /*user_data*/)
	{
		finder_called = true;
		[name]() {
			QCOMPARE(name, "fake_crs");
		}();
		return nullptr;
	}
}

void GeoreferencingTest::testProjContextSetFileFinder()
{
	finder_called = false;
	
	proj_context_set_file_finder(nullptr, &projFinderTestFakeCRS, nullptr);
	QVERIFY(!finder_called);
	
	Georeferencing fake_georef;
	fake_georef.setProjectedCRS(QStringLiteral("Fake CRS"), QStringLiteral("+init=fake_crs:123"));
	QVERIFY(finder_called);
}

#endif



QTEST_GUILESS_MAIN(GeoreferencingTest)
