/******************************************************************************
 * Project:  geography network utility
 * Purpose:  Analyse GNM networks
 * Authors:  Mikhail Gusev, gusevmihs at gmail dot com
 *           Dmitry Baryshnikov, polimax@mail.ru
 *
 ******************************************************************************
 * Copyright (C) 2014 Mikhail Gusev
 * Copyright (c) 2014-2015, NextGIS <info@nextgis.com>
 *
 * SPDX-License-Identifier: MIT
 ****************************************************************************/

#include "commonutils.h"
#include "gdal_version.h"
#include "gnm.h"
#include "gnm_priv.h"
#include "ogr_p.h"

enum operation
{
    op_unknown = 0, /** no operation */
    op_dijkstra,    /** create shortest path using Dijkstra algorithm */
    op_kpaths,      /** create k shortest paths using Yens algorithm */
    op_resource     /** create resource distribution layer */
};

/************************************************************************/
/*                               Usage()                                */
/************************************************************************/
static void Usage(bool bIsError, const char *pszAdditionalMsg = nullptr,
                  bool bShort = true, bool bHelpDoc = false)
{
    fprintf(
        bIsError ? stderr : stdout,
        "Usage: gnmanalyse [--help][--help-general][-q][-quiet][--long-usage]\n"
        "                  [dijkstra <start_gfid> <end_gfid "
        "[-alo <NAME>=<VALUE>]...]\n"
        "                  [kpaths <start_gfid> <end_gfid> <k> "
        "[-alo NAME=VALUE]...]\n"
        "                  [resource [-alo <NAME>=<VALUE>]...]\n"
        "                  [-ds <ds_name>][-f <ds_format>][-l <layer_name>]\n"
        "                  [-dsco <NAME>=<VALUE>]... [-lco <NAME>=<VALUE>]...\n"
        "                  <gnm_name>\n");

    if (bHelpDoc)
    {
        exit(0);
    }

    if (bShort)
    {
        printf("\nNote: gnmanalyse --long-usage for full help.\n");
        if (pszAdditionalMsg)
            fprintf(stderr, "\nFAILURE: %s\n", pszAdditionalMsg);
        exit(1);
    }

    fprintf(bIsError ? stderr : stdout,
            "\n   dijkstra start_gfid end_gfid: calculates the best path "
            "between two points using Dijkstra algorithm from start_gfid point "
            "to end_gfid point\n"
            "   kpaths start_gfid end_gfid k: calculates k (up to 10) best "
            "paths between two points using Yen\'s algorithm (which internally "
            "uses Dijkstra algorithm for single path calculating) from "
            "start_gfid point to end_gfid point\n"
            "   resource: calculates the \"resource distribution\". The "
            "connected components search is performed using breadth-first "
            "search and starting from that features which are marked by rules "
            "as \'EMITTERS\'\n"
            "   -ds ds_name: the name&path of the dataset to save the layer "
            "with resulting paths. Not need to be existed dataset\n"
            "   -f ds_format: define this to set the format of newly created "
            "dataset\n"
            "   -l layer_name: the name of the resulting layer. If the layer "
            "exists already - it will be rewritten. For K shortest paths "
            "several layers are created in format layer_nameN, where N - is "
            "number of the path (0 - is the most shortest one)\n"
            "   -dsco NAME=VALUE: Dataset creation option (format specific)\n"
            "   -lco  NAME=VALUE: Layer creation option (format specific)\n"
            "   -alo  NAME=VALUE: Algorithm option (format specific)\n"
            "   gnm_name: the network to work with (path and name)\n");

    if (pszAdditionalMsg)
        fprintf(stderr, "\nFAILURE: %s\n", pszAdditionalMsg);

    exit(bIsError ? 1 : 0);
}

/************************************************************************/
/*                   GetLayerAndOverwriteIfNecessary()                  */
/************************************************************************/

static OGRLayer *GetLayerAndOverwriteIfNecessary(GDALDataset *poDstDS,
                                                 const char *pszNewLayerName,
                                                 int bOverwrite,
                                                 int *pbErrorOccurred)
{
    if (pbErrorOccurred)
        *pbErrorOccurred = FALSE;

    /* GetLayerByName() can instantiate layers that would have been */
    /* 'hidden' otherwise, for example, non-spatial tables in a */
    /* PostGIS-enabled database, so this apparently useless command is */
    /* not useless... (#4012) */
    CPLPushErrorHandler(CPLQuietErrorHandler);
    OGRLayer *poDstLayer = poDstDS->GetLayerByName(pszNewLayerName);
    CPLPopErrorHandler();
    CPLErrorReset();

    int iLayer = -1;
    if (poDstLayer != nullptr)
    {
        int nLayerCount = poDstDS->GetLayerCount();
        for (iLayer = 0; iLayer < nLayerCount; iLayer++)
        {
            OGRLayer *poLayer = poDstDS->GetLayer(iLayer);
            if (poLayer == poDstLayer)
                break;
        }

        if (iLayer == nLayerCount)
            /* should not happen with an ideal driver */
            poDstLayer = nullptr;
    }

    /* -------------------------------------------------------------------- */
    /*      If the user requested overwrite, and we have the layer in       */
    /*      question we need to delete it now so it will get recreated      */
    /*      (overwritten).                                                  */
    /* -------------------------------------------------------------------- */
    if (poDstLayer != nullptr && bOverwrite)
    {
        if (poDstDS->DeleteLayer(iLayer) != OGRERR_NONE)
        {
            fprintf(stderr, "DeleteLayer() failed when overwrite requested.\n");
            if (pbErrorOccurred)
                *pbErrorOccurred = TRUE;
        }
        poDstLayer = nullptr;
    }

    return poDstLayer;
}

/************************************************************************/
/*                     CreateAndFillOutputDataset                       */
/************************************************************************/
static OGRErr CreateAndFillOutputDataset(OGRLayer *poSrcLayer,
                                         const char *pszDestDataSource,
                                         const char *pszFormat,
                                         const char *pszLayer, char **papszDSCO,
                                         char **papszLCO, int bQuiet)
{
    GDALDriver *poDriver = GetGDALDriverManager()->GetDriverByName(pszFormat);
    if (poDriver == nullptr)
    {
        fprintf(stderr, "%s driver not available\n", pszFormat);
        return OGRERR_FAILURE;
    }

    if (!CPLTestBool(CSLFetchNameValueDef(poDriver->GetMetadata(),
                                          GDAL_DCAP_CREATE, "FALSE")))
    {
        fprintf(stderr, "%s driver does not support data source creation.\n",
                pszFormat);
        return OGRERR_FAILURE;
    }

    GDALDataset *poODS =
        poDriver->Create(pszDestDataSource, 0, 0, 0, GDT_Unknown, papszDSCO);
    if (poODS == nullptr)
    {
        fprintf(stderr, "%s driver failed to create %s\n", pszFormat,
                pszDestDataSource);
        return OGRERR_FAILURE;
    }

    if (nullptr == pszLayer)
        pszLayer = poSrcLayer->GetName();
    int nError;
    GetLayerAndOverwriteIfNecessary(poODS, pszLayer, TRUE, &nError);
    if (nError == TRUE)
    {
        return OGRERR_FAILURE;
    }

    // create layer
    OGRLayer *poLayer = poODS->CopyLayer(poSrcLayer, pszLayer, papszLCO);
    if (nullptr == poLayer)
    {
        fprintf(stderr, "\nFAILURE: Can not copy path to %s\n",
                pszDestDataSource);
        GDALClose(poODS);

        return OGRERR_FAILURE;
    }

    if (bQuiet == FALSE)
    {
        printf("\nPath successfully copied and added to the network at %s\n",
               pszDestDataSource);
    }

    GDALClose(poODS);

    return OGRERR_NONE;
}

/************************************************************************/
/*                           ReportOnLayer()                            */
/************************************************************************/

static void ReportOnLayer(OGRLayer *poLayer, int bVerbose)

{
    OGRFeatureDefn *poDefn = poLayer->GetLayerDefn();

    /* -------------------------------------------------------------------- */
    /*      Report various overall information.                             */
    /* -------------------------------------------------------------------- */
    printf("\n");

    printf("Layer name: %s\n", poLayer->GetName());

    if (bVerbose)
    {
        int nGeomFieldCount = poLayer->GetLayerDefn()->GetGeomFieldCount();
        if (nGeomFieldCount > 1)
        {
            for (int iGeom = 0; iGeom < nGeomFieldCount; iGeom++)
            {
                OGRGeomFieldDefn *poGFldDefn =
                    poLayer->GetLayerDefn()->GetGeomFieldDefn(iGeom);
                printf("Geometry (%s): %s\n", poGFldDefn->GetNameRef(),
                       OGRGeometryTypeToName(poGFldDefn->GetType()));
            }
        }
        else
        {
            printf("Geometry: %s\n",
                   OGRGeometryTypeToName(poLayer->GetGeomType()));
        }

        printf("Feature Count: " CPL_FRMT_GIB "\n", poLayer->GetFeatureCount());

        OGREnvelope oExt;
        if (nGeomFieldCount > 1)
        {
            for (int iGeom = 0; iGeom < nGeomFieldCount; iGeom++)
            {
                if (poLayer->GetExtent(iGeom, &oExt, TRUE) == OGRERR_NONE)
                {
                    OGRGeomFieldDefn *poGFldDefn =
                        poLayer->GetLayerDefn()->GetGeomFieldDefn(iGeom);
                    CPLprintf("Extent (%s): (%f, %f) - (%f, %f)\n",
                              poGFldDefn->GetNameRef(), oExt.MinX, oExt.MinY,
                              oExt.MaxX, oExt.MaxY);
                }
            }
        }
        else if (poLayer->GetExtent(&oExt, TRUE) == OGRERR_NONE)
        {
            CPLprintf("Extent: (%f, %f) - (%f, %f)\n", oExt.MinX, oExt.MinY,
                      oExt.MaxX, oExt.MaxY);
        }

        char *pszWKT;

        if (nGeomFieldCount > 1)
        {
            for (int iGeom = 0; iGeom < nGeomFieldCount; iGeom++)
            {
                OGRGeomFieldDefn *poGFldDefn =
                    poLayer->GetLayerDefn()->GetGeomFieldDefn(iGeom);
                const OGRSpatialReference *poSRS = poGFldDefn->GetSpatialRef();
                if (poSRS == nullptr)
                    pszWKT = CPLStrdup("(unknown)");
                else
                {
                    poSRS->exportToPrettyWkt(&pszWKT);
                }

                printf("SRS WKT (%s):\n%s\n", poGFldDefn->GetNameRef(), pszWKT);
                CPLFree(pszWKT);
            }
        }
        else
        {
            if (poLayer->GetSpatialRef() == nullptr)
                pszWKT = CPLStrdup("(unknown)");
            else
            {
                poLayer->GetSpatialRef()->exportToPrettyWkt(&pszWKT);
            }

            printf("Layer SRS WKT:\n%s\n", pszWKT);
            CPLFree(pszWKT);
        }

        if (strlen(poLayer->GetFIDColumn()) > 0)
            printf("FID Column = %s\n", poLayer->GetFIDColumn());

        for (int iGeom = 0; iGeom < nGeomFieldCount; iGeom++)
        {
            OGRGeomFieldDefn *poGFldDefn =
                poLayer->GetLayerDefn()->GetGeomFieldDefn(iGeom);
            if (nGeomFieldCount == 1 && EQUAL(poGFldDefn->GetNameRef(), "") &&
                poGFldDefn->IsNullable())
                break;
            printf("Geometry Column ");
            if (nGeomFieldCount > 1)
                printf("%d ", iGeom + 1);
            if (!poGFldDefn->IsNullable())
                printf("NOT NULL ");
            printf("= %s\n", poGFldDefn->GetNameRef());
        }

        for (int iAttr = 0; iAttr < poDefn->GetFieldCount(); iAttr++)
        {
            OGRFieldDefn *poField = poDefn->GetFieldDefn(iAttr);
            const char *pszType =
                (poField->GetSubType() != OFSTNone)
                    ? CPLSPrintf(
                          "%s(%s)",
                          poField->GetFieldTypeName(poField->GetType()),
                          poField->GetFieldSubTypeName(poField->GetSubType()))
                    : poField->GetFieldTypeName(poField->GetType());
            printf("%s: %s (%d.%d)", poField->GetNameRef(), pszType,
                   poField->GetWidth(), poField->GetPrecision());
            if (!poField->IsNullable())
                printf(" NOT NULL");
            if (poField->GetDefault() != nullptr)
                printf(" DEFAULT %s", poField->GetDefault());
            printf("\n");
        }
    }

    /* -------------------------------------------------------------------- */
    /*      Read, and dump features.                                        */
    /* -------------------------------------------------------------------- */
    for (auto &poFeature : poLayer)
    {
        poFeature->DumpReadable(nullptr);
    }
}

/************************************************************************/
/*                                main()                                */
/************************************************************************/

#define CHECK_HAS_ENOUGH_ADDITIONAL_ARGS(nExtraArg)                            \
    do                                                                         \
    {                                                                          \
        if (iArg + nExtraArg >= nArgc)                                         \
            Usage(true, CPLSPrintf("%s option requires %d argument(s)",        \
                                   papszArgv[iArg], nExtraArg));               \
    } while (false)

MAIN_START(nArgc, papszArgv)

{
    int bQuiet = FALSE;

    const char *pszDataSource = nullptr;

    GNMGFID nFromFID = -1;
    GNMGFID nToFID = -1;
    int nK = 1;
    const char *pszDataset = nullptr;
    const char *pszFormat = "ESRI Shapefile";
    const char *pszLayer = nullptr;
    GNMNetwork *poDS = nullptr;
    OGRLayer *poResultLayer = nullptr;
    char **papszDSCO = nullptr, **papszLCO = nullptr, **papszALO = nullptr;

    operation stOper = op_unknown;

    int nRet = 0;

    // Check strict compilation and runtime library version as we use C++ API
    if (!GDAL_CHECK_VERSION(papszArgv[0]))
        exit(1);

    EarlySetConfigOptions(nArgc, papszArgv);

    /* -------------------------------------------------------------------- */
    /*      Register format(s).                                             */
    /* -------------------------------------------------------------------- */
    GDALAllRegister();

    /* -------------------------------------------------------------------- */
    /*      Processing command line arguments.                              */
    /* -------------------------------------------------------------------- */
    nArgc = GDALGeneralCmdLineProcessor(nArgc, &papszArgv, GDAL_OF_GNM);

    if (nArgc < 1)
    {
        exit(-nArgc);
    }

    for (int iArg = 1; iArg < nArgc; iArg++)
    {
        if (EQUAL(papszArgv[1], "--utility_version"))
        {
            printf("%s was compiled against GDAL %s and is running against "
                   "GDAL %s\n",
                   papszArgv[0], GDAL_RELEASE_NAME,
                   GDALVersionInfo("RELEASE_NAME"));
            CSLDestroy(papszArgv);
            return 0;
        }

        else if (EQUAL(papszArgv[iArg], "--help"))
        {
            Usage(false);
        }

        else if (EQUAL(papszArgv[iArg], "--help-doc"))
        {
            Usage(false, nullptr, true, true);
        }

        else if (EQUAL(papszArgv[iArg], "--long-usage"))
        {
            Usage(false, nullptr, false);
        }

        else if (EQUAL(papszArgv[iArg], "-q") ||
                 EQUAL(papszArgv[iArg], "-quiet"))
        {
            bQuiet = TRUE;
        }

        else if (EQUAL(papszArgv[iArg], "dijkstra"))
        {
            CHECK_HAS_ENOUGH_ADDITIONAL_ARGS(2);
            stOper = op_dijkstra;
            nFromFID = atoi(papszArgv[++iArg]);
            nToFID = atoi(papszArgv[++iArg]);
        }

        else if (EQUAL(papszArgv[iArg], "kpaths"))
        {
            CHECK_HAS_ENOUGH_ADDITIONAL_ARGS(3);
            stOper = op_kpaths;
            nFromFID = atoi(papszArgv[++iArg]);
            nToFID = atoi(papszArgv[++iArg]);
            nK = atoi(papszArgv[++iArg]);
        }

        else if (EQUAL(papszArgv[iArg], "resource"))
        {
            stOper = op_resource;
        }

        else if (EQUAL(papszArgv[iArg], "-ds"))
        {
            CHECK_HAS_ENOUGH_ADDITIONAL_ARGS(1);
            pszDataset = papszArgv[++iArg];
        }

        else if ((EQUAL(papszArgv[iArg], "-f") ||
                  EQUAL(papszArgv[iArg], "-of")))
        {
            CHECK_HAS_ENOUGH_ADDITIONAL_ARGS(1);
            pszFormat = papszArgv[++iArg];
        }

        else if (EQUAL(papszArgv[iArg], "-l"))
        {
            CHECK_HAS_ENOUGH_ADDITIONAL_ARGS(1);
            pszLayer = papszArgv[++iArg];
        }
        else if (EQUAL(papszArgv[iArg], "-dsco"))
        {
            CHECK_HAS_ENOUGH_ADDITIONAL_ARGS(1);
            papszDSCO = CSLAddString(papszDSCO, papszArgv[++iArg]);
        }
        else if (EQUAL(papszArgv[iArg], "-lco"))
        {
            CHECK_HAS_ENOUGH_ADDITIONAL_ARGS(1);
            papszLCO = CSLAddString(papszLCO, papszArgv[++iArg]);
        }
        else if (EQUAL(papszArgv[iArg], "-alo"))
        {
            CHECK_HAS_ENOUGH_ADDITIONAL_ARGS(1);
            papszALO = CSLAddString(papszALO, papszArgv[++iArg]);
        }
        else if (papszArgv[iArg][0] == '-')
        {
            Usage(true,
                  CPLSPrintf("Unknown option name '%s'", papszArgv[iArg]));
        }

        else if (pszDataSource == nullptr)
            pszDataSource = papszArgv[iArg];
    }

    // do the work
    // ////////////////////////////////////////////////////////////////

    if (stOper == op_dijkstra)
    {
        if (pszDataSource == nullptr)
            Usage(true, "No network dataset provided");

        if (nFromFID == -1 || nToFID == -1)
            Usage(true, "Invalid input from or to identificators");

        // open
        poDS = cpl::down_cast<GNMNetwork *>(static_cast<GDALDataset *>(
            GDALOpenEx(pszDataSource, GDAL_OF_UPDATE | GDAL_OF_GNM, nullptr,
                       nullptr, nullptr)));
        if (nullptr == poDS)
        {
            fprintf(stderr, "\nFailed to open network at %s\n", pszDataSource);
            nRet = 1;
            goto exit;
        }

        poResultLayer =
            poDS->GetPath(nFromFID, nToFID, GATDijkstraShortestPath, papszALO);
        if (nullptr == pszDataset)
        {
            ReportOnLayer(poResultLayer, bQuiet == FALSE);
        }
        else
        {
            if (CreateAndFillOutputDataset(poResultLayer, pszDataset, pszFormat,
                                           pszLayer, papszDSCO, papszLCO,
                                           bQuiet) != OGRERR_NONE)
            {
                nRet = 1;
                goto exit;
            }
        }
    }
    else if (stOper == op_kpaths)
    {
        if (pszDataSource == nullptr)
            Usage(true, "No network dataset provided");

        if (nFromFID == -1 || nToFID == -1)
            Usage(true, "Invalid input from or to identificators");

        // open
        poDS = cpl::down_cast<GNMNetwork *>(static_cast<GDALDataset *>(
            GDALOpenEx(pszDataSource, GDAL_OF_UPDATE | GDAL_OF_GNM, nullptr,
                       nullptr, nullptr)));
        if (nullptr == poDS)
        {
            fprintf(stderr, "\nFailed to open network at %s\n", pszDataSource);
            nRet = 1;
            goto exit;
        }

        if (CSLFindName(papszALO, GNM_MD_NUM_PATHS) == -1)
        {
            CPLDebug("GNM", "No K in options, add %d value", nK);
            papszALO = CSLAddNameValue(papszALO, GNM_MD_NUM_PATHS,
                                       CPLSPrintf("%d", nK));
        }

        poResultLayer =
            poDS->GetPath(nFromFID, nToFID, GATKShortestPath, papszALO);

        if (nullptr == pszDataset)
        {
            ReportOnLayer(poResultLayer, bQuiet == FALSE);
        }
        else
        {
            if (CreateAndFillOutputDataset(poResultLayer, pszDataset, pszFormat,
                                           pszLayer, papszDSCO, papszLCO,
                                           bQuiet) != OGRERR_NONE)
            {
                nRet = 1;
                goto exit;
            }
        }
    }
    else if (stOper == op_resource)
    {
        if (pszDataSource == nullptr)
            Usage(true, "No network dataset provided");

        // open
        poDS = cpl::down_cast<GNMNetwork *>(static_cast<GDALDataset *>(
            GDALOpenEx(pszDataSource, GDAL_OF_UPDATE | GDAL_OF_GNM, nullptr,
                       nullptr, nullptr)));
        if (nullptr == poDS)
        {
            fprintf(stderr, "\nFailed to open network at %s\n", pszDataSource);
            nRet = 1;
            goto exit;
        }

        poResultLayer =
            poDS->GetPath(nFromFID, nToFID, GATConnectedComponents, papszALO);

        if (nullptr == pszDataset)
        {
            ReportOnLayer(poResultLayer, bQuiet == FALSE);
        }
        else
        {
            if (CreateAndFillOutputDataset(poResultLayer, pszDataset, pszFormat,
                                           pszLayer, papszDSCO, papszLCO,
                                           bQuiet) != OGRERR_NONE)
            {
                nRet = 1;
                goto exit;
            }
        }
    }
    else
    {
        fprintf(
            stderr,
            "Need an operation. See help what you can do with gnmanalyse:\n");
        Usage(true);
    }

exit:
    CSLDestroy(papszDSCO);
    CSLDestroy(papszLCO);
    CSLDestroy(papszALO);
    CSLDestroy(papszArgv);

    if (poResultLayer != nullptr)
        poDS->ReleaseResultSet(poResultLayer);

    if (poDS)
    {
        if (GDALClose(poDS) != CE_None)
            nRet = 1;
    }

    GDALDestroyDriverManager();

    return nRet;
}

MAIN_END
