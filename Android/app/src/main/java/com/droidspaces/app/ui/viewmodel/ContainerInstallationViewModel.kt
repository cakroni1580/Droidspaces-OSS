package com.droidspaces.app.ui.viewmodel

import android.net.Uri
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.setValue
import androidx.lifecycle.ViewModel
import com.droidspaces.app.util.ContainerInfo
import com.droidspaces.app.util.ContainerManager
import com.droidspaces.app.util.ContainerStatus
import com.droidspaces.app.util.Constants

import com.droidspaces.app.util.ContainerConfigState
import com.droidspaces.app.util.ValidationUtils
import com.droidspaces.app.util.withConfig

class ContainerInstallationViewModel : ViewModel() {
    var tarballUri: Uri? by mutableStateOf(null)
        private set

    var containerName: String by mutableStateOf("")
        private set

    var hostname: String by mutableStateOf("")
        private set

    var useSparseImage: Boolean by mutableStateOf(true)
        private set

    var sparseImageSizeGB: Int by mutableStateOf(8)
        private set

    /**
     * Destination volume for the rootfs, or null for the default location under
     * CONTAINERS_BASE_PATH. Only the bulk data moves; config and .env stay internal.
     */
    var storageDir: String? by mutableStateOf(null)
        private set

    /** All editable networking/security/advanced config, hoisted as one value. */
    var configState: ContainerConfigState by mutableStateOf(ContainerConfigState())
        private set

    fun setTarball(uri: Uri) {
        tarballUri = uri
    }

    fun setName(name: String, hostname: String) {
        this.containerName = name
        this.hostname = hostname
    }

    fun setSparseImageConfig(useSparseImage: Boolean, sizeGB: Int, storageDir: String? = null) {
        this.useSparseImage = useSparseImage
        this.sparseImageSizeGB = sizeGB
        this.storageDir = storageDir
    }

    fun setConfig(config: ContainerConfigState) {
        this.configState = config
    }

    fun buildConfig(): ContainerInfo? {
        if (tarballUri == null) return null
        if (containerName.isEmpty()) return null

        return ContainerInfo(
            name = containerName,
            hostname = hostname.ifEmpty { ValidationUtils.sanitizeHostname(containerName) },
            rootfsPath = if (useSparseImage) {
                ContainerManager.getSparseImagePath(containerName, storageDir)
            } else {
                ContainerManager.getRootfsPath(containerName, storageDir)
            },
            status = ContainerStatus.STOPPED, // Default status for new container
            useSparseImage = useSparseImage,
            sparseImageSizeGB = if (useSparseImage) sparseImageSizeGB else null,
        ).withConfig(configState)
    }

    fun reset() {
        tarballUri = null
        containerName = ""
        hostname = ""
        useSparseImage = true
        sparseImageSizeGB = 8
        storageDir = null
        configState = ContainerConfigState()
    }
}

